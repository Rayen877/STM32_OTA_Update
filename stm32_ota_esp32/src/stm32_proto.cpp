#include "stm32_proto.h"
#include "config.h"
#include "crc32.h"

#include <Wire.h>
#include <string.h>

static constexpr uint16_t BL_MAGIC = 0xA55A;
static constexpr uint8_t CMD_HELLO = 0x01;
static constexpr uint8_t CMD_START_UPDATE = 0x10;
static constexpr uint8_t CMD_DATA = 0x11;
static constexpr uint8_t CMD_FINISH = 0x12;
static constexpr uint8_t CMD_EXECUTE = 0x14;
static constexpr uint8_t CMD_ABORT = 0x7F;
static constexpr uint8_t STATUS_OK = 0x00;
static constexpr uint16_t MAX_PAYLOAD = 1024;
static constexpr uint32_t RX_TIMEOUT = 15000;
static constexpr uint32_t HELLO_ACK_TIMEOUT = 500;
static constexpr uint32_t POLL_INTERVAL_MS = 5;

#pragma pack(push, 1)
struct ProtocolHeader {
    uint16_t magic;
    uint8_t command;
    uint8_t flags;
    uint16_t payload_length;
    uint32_t sequence;
    uint32_t offset;
    uint32_t payload_crc32;
};

struct AckPayload {
    uint8_t status;
    uint8_t reserved[3];
    uint32_t sequence;
    uint32_t next_offset;
};

struct StartUpdatePayload {
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t metadata_offset;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
    uint16_t reserved;
    char hardware_id[16];
    char build_id[16];
};
#pragma pack(pop)

static_assert(sizeof(ProtocolHeader) == 18, "ProtocolHeader must be 18 bytes");
static_assert(sizeof(AckPayload) == 12, "AckPayload must be 12 bytes");
static_assert(sizeof(StartUpdatePayload) == 52, "StartUpdatePayload must be 52 bytes");

static uint32_t controlSequence = 0;

static constexpr size_t RESP_PAYLOAD_MAX = STM32_I2C_RESP_PAYLOAD_MAX;
static constexpr size_t RESP_SIZE = sizeof(ProtocolHeader) + RESP_PAYLOAD_MAX;

static int activeBoard = 0;

static const Stm32Board& board()
{
    return STM32_BOARDS[activeBoard];
}

/*
 * I2C transport
 * -------------
 * Request : header (18 bytes) in one write, then the payload in
 *           writes of STM32_I2C_TX_CHUNK bytes (last one = remainder).
 * Response: the STM32 always answers a read of RESP_SIZE bytes.
 *           First two bytes 5A A5 = valid frame, anything else =
 *           "not ready yet" -> read again.
 */

static bool i2cWrite(const uint8_t* data, size_t length)
{
    Wire.beginTransmission(board().i2cAddr);
    Wire.write(data, length);
    uint8_t err = Wire.endTransmission(true);

    if (err != 0) {
        Serial.printf("[STM32] I2C write to 0x%02X failed (err %u)\n",
                      board().i2cAddr, err);
        return false;
    }
    return true;
}

static void i2cRestart()
{
    Wire.end();
    delay(600);                 /* > STM32 half-frame timeout (500 ms): it resynchronises */
    Wire.begin(STM32_I2C_SDA_PIN, STM32_I2C_SCL_PIN, STM32_I2C_FREQ);
    Wire.setTimeOut(200);
}

static bool sendFrameOnce(uint8_t command, uint32_t sequence, uint32_t offset,
                          const uint8_t* payload, uint16_t length)
{
    if (length > MAX_PAYLOAD) return false;

    ProtocolHeader header{};
    header.magic = BL_MAGIC;
    header.command = command;
    header.payload_length = length;
    header.sequence = sequence;
    header.offset = offset;
    header.payload_crc32 = (length && payload)
        ? crc32_compute(payload, length)
        : 0;

    if (!i2cWrite(reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        Serial.printf("[STM32] header write failed (cmd 0x%02X seq %lu)\n",
                      command, (unsigned long)sequence);
        return false;
    }

    if (length && payload) {
        size_t sent = 0;
        while (sent < length) {
            delay(1);           /* let the STM32 re-arm between transactions */

            size_t n = length - sent;
            if (n > STM32_I2C_TX_CHUNK) n = STM32_I2C_TX_CHUNK;

            if (!i2cWrite(payload + sent, n)) {
                Serial.printf("[STM32] payload write failed at byte %u/%u (seq %lu)\n",
                              (unsigned)sent, (unsigned)length, (unsigned long)sequence);
                return false;
            }
            sent += n;
        }
    }

    return true;
}

/*
 * attempts > 1: if the transfer fails, restart the I2C driver, wait for
 * the STM32 to drop the half-received frame, and send the frame again.
 */
static bool sendFrame(uint8_t command, uint32_t sequence, uint32_t offset,
                      const uint8_t* payload, uint16_t length, int attempts = 3)
{
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (sendFrameOnce(command, sequence, offset, payload, length)) return true;

        if (attempt < attempts) {
            Serial.printf("[STM32] resending frame (attempt %d/%d)\n", attempt + 1, attempts);
            i2cRestart();
        }
    }
    return false;
}

static bool receiveFrame(ProtocolHeader& header, uint8_t* payload,
                         uint16_t payloadCapacity, uint32_t timeout)
{
    uint8_t buf[RESP_SIZE];
    uint32_t start = millis();

    do {
        size_t got = Wire.requestFrom(static_cast<int>(board().i2cAddr),
                                      static_cast<int>(RESP_SIZE));

        if (got == RESP_SIZE) {
            for (size_t i = 0; i < RESP_SIZE; ++i) {
                buf[i] = static_cast<uint8_t>(Wire.read());
            }

            if (buf[0] == 0x5A && buf[1] == 0xA5) {
                memcpy(&header, buf, sizeof(header));

                if (header.magic != BL_MAGIC ||
                    header.payload_length > payloadCapacity ||
                    header.payload_length > RESP_PAYLOAD_MAX)
                    return false;

                if (header.payload_length) {
                    memcpy(payload, buf + sizeof(header), header.payload_length);
                    if (crc32_compute(payload, header.payload_length) != header.payload_crc32)
                        return false;
                }
                return true;
            }
            /* else: STM32 still busy, ask again */
        } else {
            while (Wire.available()) Wire.read();
        }

        delay(POLL_INTERVAL_MS);
    } while (millis() - start < timeout);

    return false;
}

static bool waitAck(uint8_t expectedCommand, uint32_t expectedSequence,
                    uint32_t expectedOffset, uint32_t timeout = RX_TIMEOUT)
{
    ProtocolHeader header{};
    uint8_t payload[64]{};

    if (!receiveFrame(header, payload, sizeof(payload), timeout)) {
        Serial.println("[STM32] ACK timeout");
        return false;
    }

    if (header.command != static_cast<uint8_t>(expectedCommand | 0x80)) {
        Serial.printf("[STM32] Unexpected response command: 0x%02X\n", header.command);
        return false;
    }

    if (header.payload_length != sizeof(AckPayload)) {
        Serial.println("[STM32] Invalid ACK length");
        return false;
    }

    AckPayload ack{};
    memcpy(&ack, payload, sizeof(ack));

    if (ack.status != STATUS_OK) {
        Serial.printf("[STM32] Bootloader error: 0x%02X\n", ack.status);
        return false;
    }

    if (ack.sequence != expectedSequence || ack.next_offset != expectedOffset) {
        Serial.printf("[STM32] ACK mismatch seq=%lu/%lu offset=%lu/%lu\n",
                      (unsigned long)ack.sequence,
                      (unsigned long)expectedSequence,
                      (unsigned long)ack.next_offset,
                      (unsigned long)expectedOffset);
        return false;
    }

    return true;
}

void stm32ProtocolBegin()
{
    /* NRST lines: open-drain, released (HIGH) */
    for (size_t i = 0; i < STM32_BOARD_COUNT; ++i) {
        if (STM32_BOARDS[i].resetPin >= 0) {
            pinMode(STM32_BOARDS[i].resetPin, OUTPUT_OPEN_DRAIN);
            digitalWrite(STM32_BOARDS[i].resetPin, HIGH);
        }
    }

    Wire.begin(STM32_I2C_SDA_PIN, STM32_I2C_SCL_PIN, STM32_I2C_FREQ);
    Wire.setTimeOut(200);

    Serial.printf("[STM32] I2C SDA=%d SCL=%d freq=%lu, %u board(s)\n",
                  STM32_I2C_SDA_PIN, STM32_I2C_SCL_PIN,
                  (unsigned long)STM32_I2C_FREQ,
                  (unsigned)STM32_BOARD_COUNT);

    for (size_t i = 0; i < STM32_BOARD_COUNT; ++i) {
        Serial.printf("[STM32]   %s -> I2C 0x%02X, NRST GPIO %d\n",
                      STM32_BOARDS[i].id,
                      STM32_BOARDS[i].i2cAddr,
                      STM32_BOARDS[i].resetPin);
    }
}

bool stm32SelectBoardByIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(STM32_BOARD_COUNT)) return false;
    activeBoard = index;
    return true;
}

bool stm32SelectBoardById(const char* id)
{
    if (!id) return false;

    for (size_t i = 0; i < STM32_BOARD_COUNT; ++i) {
        if (strcmp(STM32_BOARDS[i].id, id) == 0) {
            activeBoard = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

const char* stm32ActiveBoardId()
{
    return board().id;
}

bool stm32EnterBootloader()
{
    const Stm32Board& b = board();

    Serial.printf("[STM32] Board %s (I2C 0x%02X): resetting...\n", b.id, b.i2cAddr);

    if (b.resetPin >= 0) {
        digitalWrite(b.resetPin, LOW);
        delay(100);
        digitalWrite(b.resetPin, HIGH);      /* open-drain: released */
        delay(200);
    } else {
        Serial.println("[STM32] No reset pin configured: reset the board by hand now");
    }

    Serial.println("[STM32] Waiting for bootloader...");

    uint32_t start = millis();
    uint32_t seq = 0;

    while (millis() - start < 5000) {
        Serial.printf("[STM32] Sending HELLO seq=%lu\n", (unsigned long)seq);

        if (sendFrame(CMD_HELLO, seq, 0, nullptr, 0, 1)) {
            if (waitAck(CMD_HELLO, seq, 0, HELLO_ACK_TIMEOUT)) {
                Serial.println("[STM32] Bootloader detected");
                return true;
            }
        }

        seq++;
        delay(100);
    }

    Serial.println("[STM32] Bootloader not detected");
    return false;
}

bool stm32SendStart(uint32_t imageSize, uint32_t imageCrc,
                    uint32_t metadataOffset, uint16_t versionMajor,
                    uint16_t versionMinor, uint16_t versionPatch)
{
    StartUpdatePayload payload{};
    payload.image_size = imageSize;
    payload.image_crc32 = imageCrc;
    payload.metadata_offset = metadataOffset;
    payload.version_major = versionMajor;
    payload.version_minor = versionMinor;
    payload.version_patch = versionPatch;

    strncpy(payload.hardware_id, "STM32F401RE", sizeof(payload.hardware_id) - 1);
    strncpy(payload.build_id, "ESP32-OTA", sizeof(payload.build_id) - 1);

    uint32_t seq = ++controlSequence;

    Serial.printf("[STM32] START size=%lu crc=%08lX meta=0x%lX version=%u.%u.%u\n",
                  (unsigned long)imageSize,
                  (unsigned long)imageCrc,
                  (unsigned long)metadataOffset,
                  versionMajor, versionMinor, versionPatch);

    if (!sendFrame(CMD_START_UPDATE, seq, 0,
                   reinterpret_cast<uint8_t*>(&payload), sizeof(payload)))
        return false;

    /* the STM32 is erasing its flash and cannot answer on I2C yet */
    Serial.println("[STM32] Waiting for flash erase...");
    delay(STM32_I2C_ERASE_WAIT_MS);

    return waitAck(CMD_START_UPDATE, seq, 0);
}

bool stm32SendChunk(uint32_t sequence, uint32_t offset,
                    const uint8_t* data, uint16_t length)
{
    if (!data || length == 0 || length > MAX_PAYLOAD) return false;
    if (!sendFrame(CMD_DATA, sequence, offset, data, length)) return false;
    return waitAck(CMD_DATA, sequence, offset + length);
}

bool stm32SendFinish(uint32_t imageSize, uint32_t imageCrc)
{
    uint8_t payload[8];
    memcpy(payload, &imageSize, 4);
    memcpy(payload + 4, &imageCrc, 4);

    uint32_t seq = ++controlSequence;

    if (!sendFrame(CMD_FINISH, seq, 0, payload, sizeof(payload))) return false;
    return waitAck(CMD_FINISH, seq, imageSize);
}

bool stm32Execute(uint32_t imageSize)
{
    uint32_t seq = ++controlSequence;
    Serial.println("[STM32] Sending EXECUTE");

    if (!sendFrame(CMD_EXECUTE, seq, 0, nullptr, 0)) {
        return false;
    }

    /* STM32 returns the final received byte count in next_offset. */
    return waitAck(CMD_EXECUTE, seq, imageSize);
}

void stm32Abort()
{
    uint32_t seq = ++controlSequence;
    sendFrame(CMD_ABORT, seq, 0, nullptr, 0);
}