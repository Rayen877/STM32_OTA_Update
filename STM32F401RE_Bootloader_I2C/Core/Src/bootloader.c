#include "bootloader.h"
#include "protocol.h"
#include "flash_if.h"
#include "crc32.h"
#include "firmware_metadata.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    bool active;
    bool verified;

    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t metadata_offset;

    uint32_t expected_sequence;
    uint32_t received;

    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;

} UpdateContext;

static UpdateContext s_update;

static ProtocolFrame s_frame;

static I2C_HandleTypeDef *s_hi2c = NULL;

static bool VersionGreater(
    uint16_t a_major,
    uint16_t a_minor,
    uint16_t a_patch,
    uint16_t b_major,
    uint16_t b_minor,
    uint16_t b_patch)
{
    if (a_major != b_major)
    {
        return a_major > b_major;
    }

    if (a_minor != b_minor)
    {
        return a_minor > b_minor;
    }

    return a_patch > b_patch;
}

static bool FindMetadata(
    uint32_t metadata_offset,
    FirmwareMetadata **metadata)
{
    if (metadata_offset >
        APP_MAX_SIZE - FW_METADATA_SIZE)
    {
        return false;
    }

    FirmwareMetadata *m =
        (FirmwareMetadata *)
        (APP_ADDRESS + metadata_offset);

    if (m->magic !=
        FW_METADATA_MAGIC)
    {
        return false;
    }

    if (m->format_version !=
        FW_METADATA_FORMAT_VERSION)
    {
        return false;
    }

    if (m->header_size !=
        FW_METADATA_HEADER_SIZE)
    {
        return false;
    }

    if (m->hardware_id !=
        FW_HARDWARE_ID)
    {
        return false;
    }

    *metadata = m;

    return true;
}

static bool VerifyApplicationImage(void)
{
    FirmwareMetadata *metadata =
        NULL;

    if (!FindMetadata(
            s_update.metadata_offset,
            &metadata))
    {
        return false;
    }

    if (metadata->image_size !=
        s_update.image_size)
    {
        return false;
    }

    if (metadata->image_crc32 !=
        s_update.image_crc32)
    {
        return false;
    }

    if (metadata->version_major !=
        s_update.version_major)
    {
        return false;
    }

    if (metadata->version_minor !=
        s_update.version_minor)
    {
        return false;
    }

    if (metadata->version_patch !=
        s_update.version_patch)
    {
        return false;
    }

    /*
     * Same CRC rule as the PC:
     *
     * CRC over the whole .bin,
     * but image_crc32 itself is treated as 0.
     */
    uint32_t crc_offset =
        s_update.metadata_offset +
        offsetof(
            FirmwareMetadata,
            image_crc32
        );

    uint32_t calculated =
        CRC32_CalculateWithZeroRange(
            (const uint8_t *)APP_ADDRESS,
            s_update.image_size,
            crc_offset,
            sizeof(uint32_t)
        );

    return calculated ==
           s_update.image_crc32;
}

bool Bootloader_ApplicationIsValid(void)
{
    uint32_t initial_stack =
        *(__IO uint32_t *)
        (APP_ADDRESS + 0U);

    uint32_t reset_handler =
        *(__IO uint32_t *)
        (APP_ADDRESS + 4U);

    bool stack_valid =
        (initial_stack >= 0x20000000UL) &&
        (initial_stack <=  0x20018000UL);

    bool reset_valid =
        (reset_handler >= APP_ADDRESS) &&
        (reset_handler < APP_END_ADDRESS) &&
        ((reset_handler & 1U) != 0U);

    return stack_valid &&
           reset_valid;
}

void Bootloader_JumpToApplication(void)
{
    uint32_t app_stack =
        *(__IO uint32_t *)
        APP_ADDRESS;

    uint32_t app_reset =
        *(__IO uint32_t *)
        (APP_ADDRESS + 4U);

    typedef void (*ApplicationEntry)(void);

    ApplicationEntry application =
        (ApplicationEntry)app_reset;

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /*
     * Disable/clear NVIC interrupts.
     */
    for (uint32_t i = 0U;
         i < 8U;
         i++)
    {
        NVIC->ICER[i] =
            0xFFFFFFFFUL;

        NVIC->ICPR[i] =
            0xFFFFFFFFUL;
    }

    if (s_hi2c != NULL)
    {
        HAL_I2C_DeInit(
            s_hi2c
        );
    }

    /*
     * Application vector table.
     */
    SCB->VTOR =
        APP_ADDRESS;

    __DSB();
    __ISB();

    /*
     * Application's initial MSP.
     */
    __set_MSP(
        app_stack
    );

    __enable_irq();
    __ISB();

    application();

    while (1)
    {
    }
}

static void SendInfo(uint32_t sequence)
{
    InfoResponsePayload info = {0};

    info.status =
        BL_STATUS_OK;

    info.boot_major =
        BOOTLOADER_VERSION_MAJOR;

    info.boot_minor =
        BOOTLOADER_VERSION_MINOR;

    info.boot_patch =
        BOOTLOADER_VERSION_PATCH;

    info.app_valid =
        Bootloader_ApplicationIsValid()
        ? 1U
        : 0U;

    info.hardware_id =
        FW_HARDWARE_ID;

    /*
     * 96-bit factory-programmed unique ID of THIS chip.
     * Lets the ESP32 / dashboard know which physical
     * STM32 answered at which I2C address.
     */
    info.uid[0] =
        HAL_GetUIDw0();

    info.uid[1] =
        HAL_GetUIDw1();

    info.uid[2] =
        HAL_GetUIDw2();

    /*
     * Search the application's metadata
     * section.
     */
    if (info.app_valid)
    {
        for (uint32_t offset = 0U;
             offset <=
             APP_MAX_SIZE - FW_METADATA_SIZE;
             offset += 4U)
        {
            FirmwareMetadata *m =
                (FirmwareMetadata *)
                (APP_ADDRESS + offset);

            if ((m->magic ==
                 FW_METADATA_MAGIC) &&

                (m->format_version ==
                 FW_METADATA_FORMAT_VERSION) &&

                (m->header_size ==
                 FW_METADATA_HEADER_SIZE) &&

                (m->hardware_id ==
                 FW_HARDWARE_ID))
            {
                info.app_size =
                    m->image_size;

                info.version_major =
                    m->version_major;

                info.version_minor =
                    m->version_minor;

                info.version_patch =
                    m->version_patch;

                break;
            }
        }
    }

    Protocol_Send(
        BL_CMD_GET_INFO | 0x80U,
        sequence,
        0U,
        (uint8_t *)&info,
        sizeof(info)
    );
}

static uint8_t HandleStartUpdate(
    const ProtocolFrame *frame)
{
    if (frame->header.payload_length !=
        sizeof(StartUpdatePayload))
    {
        return BL_STATUS_BAD_LENGTH;
    }

    const StartUpdatePayload *start =
        (const StartUpdatePayload *)
        frame->payload;

    if ((start->image_size == 0U) ||
        (start->image_size > APP_MAX_SIZE))
    {
        return BL_STATUS_INVALID_IMAGE;
    }

    if (start->metadata_offset >
        start->image_size -
        FW_METADATA_SIZE)
    {
        return BL_STATUS_INVALID_IMAGE;
    }

    /*
     * Hardware ID from ESP32.
     * Empty is accepted for the first integration test.
     */
    if (start->hardware_id[0] != '\0')
    {
        if (strncmp(
                start->hardware_id,
                APP_HARDWARE_ID_STRING,
                sizeof(start->hardware_id)
            ) != 0)
        {
            return BL_STATUS_HARDWARE_ERROR;
        }
    }

#if BL_ENFORCE_NEWER_VERSION

    if (Bootloader_ApplicationIsValid())
    {
        FirmwareMetadata *current = NULL;

        /*
         * Find currently installed metadata.
         */
        for (uint32_t offset = 0U;
             offset <=
             APP_MAX_SIZE - FW_METADATA_SIZE;
             offset += 4U)
        {
            FirmwareMetadata *m =
                (FirmwareMetadata *)
                (APP_ADDRESS + offset);

            if ((m->magic ==
                 FW_METADATA_MAGIC) &&

                (m->format_version ==
                 FW_METADATA_FORMAT_VERSION) &&

                (m->header_size ==
                 FW_METADATA_HEADER_SIZE) &&

                (m->hardware_id ==
                 FW_HARDWARE_ID))
            {
                current = m;
                break;
            }
        }

        if (current != NULL)
        {
            if (!VersionGreater(
                    start->version_major,
                    start->version_minor,
                    start->version_patch,

                    current->version_major,
                    current->version_minor,
                    current->version_patch))
            {
                return BL_STATUS_VERSION_ERROR;
            }
        }
    }

#endif

    /*
     * Only now erase the application.
     */
    if (FlashIF_EraseApplication()
        != HAL_OK)
    {
        return BL_STATUS_FLASH_ERROR;
    }

    memset(
        &s_update,
        0,
        sizeof(s_update)
    );

    s_update.active =
        true;

    s_update.verified =
        false;

    s_update.image_size =
        start->image_size;

    s_update.image_crc32 =
        start->image_crc32;

    s_update.metadata_offset =
        start->metadata_offset;

    s_update.version_major =
        start->version_major;

    s_update.version_minor =
        start->version_minor;

    s_update.version_patch =
        start->version_patch;

    s_update.expected_sequence =
        0U;

    s_update.received =
        0U;

    return BL_STATUS_OK;
}

static uint8_t HandleData(
    const ProtocolFrame *frame)
{
    if (!s_update.active)
    {
        return BL_STATUS_NOT_READY;
    }

    if ((frame->header.payload_length == 0U) ||
        (frame->header.payload_length >
         BL_MAX_PAYLOAD))
    {
        return BL_STATUS_BAD_LENGTH;
    }

    if (frame->header.sequence !=
        s_update.expected_sequence)
    {
        return BL_STATUS_BAD_SEQUENCE;
    }

    if (frame->header.offset !=
        s_update.received)
    {
        return BL_STATUS_BAD_SEQUENCE;
    }

    if ((s_update.received +
         frame->header.payload_length) >
        s_update.image_size)
    {
        return BL_STATUS_BAD_LENGTH;
    }

    bool last_chunk =
        (s_update.received +
         frame->header.payload_length) ==
        s_update.image_size;

    /*
     * Word programming needs aligned offsets.
     */
    if ((frame->header.offset & 3U) != 0U)
    {
        return BL_STATUS_BAD_LENGTH;
    }

    /*
     * Intermediate chunks should be multiples of
     * 4 bytes. The final chunk may be smaller.
     */
    if (!last_chunk &&
        ((frame->header.payload_length & 3U) != 0U))
    {
        return BL_STATUS_BAD_LENGTH;
    }

    if (FlashIF_Write(
            APP_ADDRESS +
            s_update.received,

            frame->payload,

            frame->header.payload_length
        ) != HAL_OK)
    {
        return BL_STATUS_FLASH_ERROR;
    }

    s_update.received +=
        frame->header.payload_length;

    s_update.expected_sequence++;

    return BL_STATUS_OK;
}

static uint8_t HandleFinish(void)
{
    if (!s_update.active)
    {
        return BL_STATUS_NOT_READY;
    }

    if (s_update.received !=
        s_update.image_size)
    {
        return BL_STATUS_INVALID_IMAGE;
    }

    if (!VerifyApplicationImage())
    {
        return BL_STATUS_INVALID_IMAGE;
    }

    s_update.verified =
        true;

    return BL_STATUS_OK;
}

static void ProcessFrame(
    const ProtocolFrame *frame)
{
    uint8_t status;

    switch (frame->header.command)
    {
        case BL_CMD_HELLO:

            Protocol_SendAck(
                BL_CMD_HELLO,
                BL_STATUS_OK,
                frame->header.sequence,
                0U
            );

            break;


        case BL_CMD_GET_INFO:

            SendInfo(
                frame->header.sequence
            );

            break;


        case BL_CMD_START_UPDATE:

            status =
                HandleStartUpdate(
                    frame
                );

            Protocol_SendAck(
                BL_CMD_START_UPDATE,
                status,
                frame->header.sequence,
                0U
            );

            break;


        case BL_CMD_DATA:

            status =
                HandleData(
                    frame
                );

            Protocol_SendAck(
                BL_CMD_DATA,
                status,
                frame->header.sequence,
                s_update.received
            );

            break;


        case BL_CMD_FINISH:

            status =
                HandleFinish();

            Protocol_SendAck(
                BL_CMD_FINISH,
                status,
                frame->header.sequence,
                s_update.received
            );

            break;


        case BL_CMD_VERIFY:

            if (!s_update.active)
            {
                status =
                    BL_STATUS_NOT_READY;
            }
            else
            {
                status =
                    VerifyApplicationImage()
                    ? BL_STATUS_OK
                    : BL_STATUS_INVALID_IMAGE;
            }

            Protocol_SendAck(
                BL_CMD_VERIFY,
                status,
                frame->header.sequence,
                s_update.received
            );

            break;


        case BL_CMD_EXECUTE:

            if (!s_update.verified)
            {
                Protocol_SendAck(
                    BL_CMD_EXECUTE,
                    BL_STATUS_INVALID_IMAGE,
                    frame->header.sequence,
                    s_update.received
                );

                break;
            }

            Protocol_SendAck(
                BL_CMD_EXECUTE,
                BL_STATUS_OK,
                frame->header.sequence,
                s_update.received
            );

            HAL_Delay(100U);

            Bootloader_JumpToApplication();

            break;


        case BL_CMD_ABORT:

            memset(
                &s_update,
                0,
                sizeof(s_update)
            );

            Protocol_SendAck(
                BL_CMD_ABORT,
                BL_STATUS_OK,
                frame->header.sequence,
                0U
            );

            break;


        default:

            Protocol_SendAck(
                frame->header.command,
                BL_STATUS_INVALID_COMMAND,
                frame->header.sequence,
                0U
            );

            break;
    }
}

void Bootloader_Run(
    I2C_HandleTypeDef *hi2c)
{
	//HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, 1);

    s_hi2c =
        hi2c;
    Protocol_Init(
        hi2c
    );

    memset(
        &s_update,
        0,
        sizeof(s_update)
    );

    uint32_t wait_start =
        HAL_GetTick();

    while (1)
    {
        uint32_t timeout;

        /*
         * During an update:
         * stay in bootloader.
         */
        if (s_update.active)
        {
            timeout = 1000U;
        }

        /*
         * If there is no valid application,
         * stay in bootloader.
         */
        else if (!Bootloader_ApplicationIsValid())
        {
            timeout = 1000U;
        }

        /*
         * Valid application:
         * wait a few seconds for ESP32.
         */
        else
        {
            uint32_t elapsed =
                HAL_GetTick() -
                wait_start;

            if (elapsed >= BOOT_WAIT_MS)
            {
                Bootloader_JumpToApplication();
            }

            timeout =
                BOOT_WAIT_MS -
                elapsed;

            if (timeout > 100U)
            {
                timeout = 100U;
            }
        }

        ProtocolResult result =
            Protocol_Receive(
                &s_frame,
                timeout
            );

        if (result ==
            PROTOCOL_TIMEOUT)
        {
            continue;
        }

        if (result ==
            PROTOCOL_BAD_CRC)
        {
            Protocol_SendAck(
                s_frame.header.command,
                BL_STATUS_BAD_CRC,
                s_frame.header.sequence,
                s_update.received
            );

            continue;
        }

        if (result ==
            PROTOCOL_BAD_LENGTH)
        {
            Protocol_SendAck(
                s_frame.header.command,
                BL_STATUS_BAD_LENGTH,
                s_frame.header.sequence,
                s_update.received
            );

            continue;
        }

        if (result ==
            PROTOCOL_OK)
        {
            ProcessFrame(
                &s_frame
            );
        }
    }
}
