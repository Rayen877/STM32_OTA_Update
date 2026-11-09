#include "protocol.h"
#include "crc32.h"

#include <string.h>

/*
 * ============================================================
 *  I2C slave transport for the bootloader protocol
 * ============================================================
 *
 * The STM32 is the I2C slave, the ESP32 is the master.
 * Frames keep the exact same layout as on the UART:
 *
 *      ProtocolHeader (18 bytes) + payload
 *
 * REQUEST  (ESP32 -> STM32), only I2C WRITE transactions:
 *
 *   1. write: the 18-byte header
 *   2. write: the payload, in chunks of BL_I2C_RX_CHUNK bytes
 *             (last chunk = whatever is left).
 *             Skipped when payload_length == 0.
 *
 * RESPONSE (STM32 -> ESP32), only I2C READ transactions:
 *
 *   read exactly BL_I2C_RESP_SIZE bytes.
 *   - first 2 bytes == 5A A5  -> valid response
 *       header (18 bytes) + payload (payload_length bytes) + padding
 *   - first 2 bytes != 5A A5  -> not ready yet (all zeros), read again
 *   The response stays readable until the next request header arrives,
 *   so the ESP32 can safely re-read it.
 */

#define BL_I2C_RX_CHUNK          64U   /* ESP32 must write payload in chunks of this size */
#define BL_I2C_RESP_PAYLOAD_MAX  48U   /* biggest payload the STM32 sends back             */
#define BL_I2C_RESP_SIZE         (sizeof(ProtocolHeader) + BL_I2C_RESP_PAYLOAD_MAX)
#define BL_I2C_FRAME_TIMEOUT_MS  500U  /* drop a half-received frame after this            */

_Static_assert(sizeof(ProtocolHeader) == 18U,
               "ProtocolHeader must be 18 bytes (packed)");
_Static_assert(sizeof(AckPayload) <= BL_I2C_RESP_PAYLOAD_MAX,
               "AckPayload too big: raise BL_I2C_RESP_PAYLOAD_MAX");
_Static_assert(sizeof(InfoResponsePayload) <= BL_I2C_RESP_PAYLOAD_MAX,
               "InfoResponsePayload too big: raise BL_I2C_RESP_PAYLOAD_MAX");

typedef enum
{
    RX_WAIT_HEADER = 0,   /* ready for a new request header          */
    RX_WAIT_PAYLOAD,      /* header received, payload still arriving */
    RX_FRAME_READY,       /* complete frame waiting for the main loop */
    RX_PROCESSING         /* main loop is working, response pending  */

} RxState;

typedef enum
{
    ARM_NONE = 0,
    ARM_HEADER,
    ARM_PAYLOAD,
    ARM_DISCARD

} ArmKind;

static I2C_HandleTypeDef *s_hi2c = NULL;

/* ---- receive side (written by the I2C interrupt) ---- */
static volatile RxState  s_rx_state    = RX_WAIT_HEADER;
static volatile ArmKind  s_armed       = ARM_NONE;
static volatile uint16_t s_armed_len   = 0U;
static volatile uint32_t s_rx_received = 0U;
static volatile uint32_t s_rx_tick     = 0U;

static ProtocolHeader s_rx_header;
static uint8_t        s_rx_payload[BL_MAX_PAYLOAD];
static uint8_t        s_discard[BL_I2C_RX_CHUNK];

/* ---- transmit side ---- */
static volatile uint8_t s_tx_ready = 0U;
static uint8_t s_txbuf[BL_I2C_RESP_SIZE];
static uint8_t s_notready[BL_I2C_RESP_SIZE];   /* stays all zeros */

void Protocol_Init(
    I2C_HandleTypeDef *hi2c)
{
    s_hi2c = hi2c;

    s_rx_state    = RX_WAIT_HEADER;
    s_armed       = ARM_NONE;
    s_rx_received = 0U;
    s_tx_ready    = 0U;

    (void)HAL_I2C_EnableListen_IT(hi2c);
}

/* ============================================================
 *  HAL I2C callbacks (run in interrupt context)
 * ============================================================ */

void HAL_I2C_ListenCpltCallback(
    I2C_HandleTypeDef *hi2c)
{
    /* Transaction finished (STOP or master NACK): listen again. */
    (void)HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_ErrorCallback(
    I2C_HandleTypeDef *hi2c)
{
    s_armed = ARM_NONE;

    (void)HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_AddrCallback(
    I2C_HandleTypeDef *hi2c,
    uint8_t TransferDirection,
    uint16_t AddrMatchCode)
{
    (void)AddrMatchCode;

    /*
     * Every branch MUST arm a transfer, otherwise the
     * peripheral keeps SCL stretched.
     */
    if (TransferDirection == I2C_DIRECTION_TRANSMIT)
    {
        /* master writes to us */
        s_rx_tick = HAL_GetTick();

        if (s_rx_state == RX_WAIT_HEADER)
        {
            /* a new request invalidates the previous response */
            s_tx_ready = 0U;

            s_armed     = ARM_HEADER;
            s_armed_len = (uint16_t)sizeof(ProtocolHeader);

            (void)HAL_I2C_Slave_Sequential_Receive_IT(
                hi2c,
                (uint8_t *)&s_rx_header,
                s_armed_len,
                I2C_FIRST_AND_LAST_FRAME);
        }
        else if (s_rx_state == RX_WAIT_PAYLOAD)
        {
            uint32_t remaining =
                (uint32_t)s_rx_header.payload_length -
                s_rx_received;

            uint32_t chunk =
                (remaining > BL_I2C_RX_CHUNK)
                ? BL_I2C_RX_CHUNK
                : remaining;

            s_armed     = ARM_PAYLOAD;
            s_armed_len = (uint16_t)chunk;

            (void)HAL_I2C_Slave_Sequential_Receive_IT(
                hi2c,
                &s_rx_payload[s_rx_received],
                s_armed_len,
                I2C_FIRST_AND_LAST_FRAME);
        }
        else
        {
            /* busy with a previous frame: swallow the bytes */
            s_armed     = ARM_DISCARD;
            s_armed_len = (uint16_t)sizeof(s_discard);

            (void)HAL_I2C_Slave_Sequential_Receive_IT(
                hi2c,
                s_discard,
                s_armed_len,
                I2C_FIRST_AND_LAST_FRAME);
        }
    }
    else
    {
        /* master reads from us */
        uint8_t *src =
            (s_tx_ready != 0U)
            ? s_txbuf
            : s_notready;

        (void)HAL_I2C_Slave_Sequential_Transmit_IT(
            hi2c,
            src,
            (uint16_t)BL_I2C_RESP_SIZE,
            I2C_FIRST_AND_LAST_FRAME);
    }
}

void HAL_I2C_SlaveRxCpltCallback(
    I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;

    s_rx_tick = HAL_GetTick();

    switch (s_armed)
    {
        case ARM_HEADER:

            if ((s_rx_header.magic == BL_MAGIC) &&
                (s_rx_header.payload_length > 0U) &&
                (s_rx_header.payload_length <= BL_MAX_PAYLOAD))
            {
                s_rx_received = 0U;
                s_rx_state    = RX_WAIT_PAYLOAD;
            }
            else
            {
                /*
                 * No payload, or invalid header:
                 * Protocol_Receive() sorts it out.
                 */
                s_rx_state = RX_FRAME_READY;
            }

            break;


        case ARM_PAYLOAD:

            s_rx_received += s_armed_len;

            if (s_rx_received >= s_rx_header.payload_length)
            {
                s_rx_state = RX_FRAME_READY;
            }

            break;


        case ARM_DISCARD:
        case ARM_NONE:
        default:

            break;
    }

    s_armed = ARM_NONE;
}

/* ============================================================
 *  I2C listen watchdog
 * ============================================================ */

static void Protocol_I2CWatchdog(void)
{
    /*
     * Peripheral idle but not listening (a re-arm was lost,
     * e.g. HAL_I2C_EnableListen_IT() returned HAL_BUSY inside
     * an error callback): listen again.
     *
     * Only the software state is read here. NEVER read the I2C
     * status registers (SR1/SR2) from the main loop: reading SR1
     * and then SR2 clears the ADDR flag by hardware, and the HAL
     * interrupt handler would then miss the address match.
     */
    if (s_hi2c->State == HAL_I2C_STATE_READY)
    {
        (void)HAL_I2C_EnableListen_IT(s_hi2c);
    }
}

/* ============================================================
 *  API used by bootloader.c
 * ============================================================ */

ProtocolResult Protocol_Receive(
    ProtocolFrame *frame,
    uint32_t timeout_ms)
{
    if ((s_hi2c == NULL) ||
        (frame == NULL))
    {
        return PROTOCOL_TIMEOUT;
    }

    uint32_t start =
        HAL_GetTick();

    do
    {
        Protocol_I2CWatchdog();

        /*
         * Half-received frame (master gave up):
         * forget it and wait for a new header.
         */
        if ((s_rx_state == RX_WAIT_PAYLOAD) &&
            ((HAL_GetTick() - s_rx_tick) >
             BL_I2C_FRAME_TIMEOUT_MS))
        {
            s_rx_state = RX_WAIT_HEADER;
        }

        if (s_rx_state == RX_FRAME_READY)
        {
            memcpy(
                &frame->header,
                &s_rx_header,
                sizeof(ProtocolHeader)
            );

            if (frame->header.magic != BL_MAGIC)
            {
                /* garbage: ignore, like the UART version did */
                s_rx_state = RX_WAIT_HEADER;
                continue;
            }

            /*
             * From here on a response is owed to the master.
             * Protocol_Send() releases the receiver again.
             */
            s_rx_state = RX_PROCESSING;

            if (frame->header.payload_length >
                BL_MAX_PAYLOAD)
            {
                return PROTOCOL_BAD_LENGTH;
            }

            if (frame->header.payload_length > 0U)
            {
                memcpy(
                    frame->payload,
                    s_rx_payload,
                    frame->header.payload_length
                );

                uint32_t crc =
                    CRC32_Calculate(
                        frame->payload,
                        frame->header.payload_length
                    );

                if (crc !=
                    frame->header.payload_crc32)
                {
                    return PROTOCOL_BAD_CRC;
                }
            }
            else
            {
                if (frame->header.payload_crc32 != 0U)
                {
                    return PROTOCOL_BAD_CRC;
                }
            }

            return PROTOCOL_OK;
        }

    } while ((HAL_GetTick() - start) < timeout_ms);

    return PROTOCOL_TIMEOUT;
}

HAL_StatusTypeDef Protocol_Send(
    uint8_t command,
    uint32_t sequence,
    uint32_t offset,
    const uint8_t *payload,
    uint16_t payload_length)
{
    if (s_hi2c == NULL)
    {
        return HAL_ERROR;
    }

    if (payload_length >
        BL_I2C_RESP_PAYLOAD_MAX)
    {
        return HAL_ERROR;
    }

    ProtocolHeader header = {0};

    header.magic =
        BL_MAGIC;

    header.command =
        command;

    header.flags =
        0U;

    header.payload_length =
        payload_length;

    header.sequence =
        sequence;

    header.offset =
        offset;

    if (payload_length > 0U)
    {
        header.payload_crc32 =
            CRC32_Calculate(
                payload,
                payload_length
            );
    }
    else
    {
        header.payload_crc32 =
            0U;
    }

    /*
     * Build the response in the TX buffer. The master
     * only reads it once s_tx_ready is set.
     */
    s_tx_ready = 0U;

    memset(s_txbuf, 0, sizeof(s_txbuf));

    memcpy(s_txbuf, &header, sizeof(header));

    if (payload_length > 0U)
    {
        memcpy(
            s_txbuf + sizeof(header),
            payload,
            payload_length
        );
    }

    __DMB();

    s_tx_ready = 1U;

    /* response is out: accept the next request */
    if (s_rx_state == RX_PROCESSING)
    {
        s_rx_state = RX_WAIT_HEADER;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Protocol_SendAck(
    uint8_t request_command,
    uint8_t status,
    uint32_t sequence,
    uint32_t next_offset)
{
    AckPayload ack = {0};

    ack.status =
        status;

    ack.sequence =
        sequence;

    ack.next_offset =
        next_offset;

    return Protocol_Send(
        request_command | 0x80U,
        sequence,
        next_offset,
        (uint8_t *)&ack,
        sizeof(ack)
    );
}
