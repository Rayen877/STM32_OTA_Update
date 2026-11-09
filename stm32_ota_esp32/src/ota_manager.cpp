#include "ota_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "crc32.h"
#include "stm32_proto.h"

static const char* STAGING_FILE = "/stm32_firmware.bin";

void OtaManager::begin(QueueHandle_t statusQueue) {
    statusQueue_ = statusQueue;
}

void OtaManager::status(const char* state, int progress, const char* detail) {
    if (!statusQueue_) return;

    JsonDocument doc;
    doc["state"] = state;
    doc["board"] = stm32ActiveBoardId();
    if (progress >= 0) doc["progress"] = progress;
    doc["received"] = received_;
    doc["expected"] = expectedSize_;
    if (detail) doc["detail"] = detail;

    if (strcmp(state, "CHUNK_ACK") == 0 && sequence_ > 0) {
        doc["seq"] = sequence_ - 1;
        doc["next_offset"] = received_;
    }

    OtaStatusMessage msg{};
    serializeJson(doc, msg.json);

    if (xQueueSend(statusQueue_, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
        Serial.println("[OTA] status queue full");
        return;
    }
    Serial.printf("[OTA] %s\n", msg.json);
}

void OtaManager::handle(const OtaCommand& cmd) {
    switch (cmd.type) {
        case OTA_CMD_START: handleStart(cmd.json); break;
        case OTA_CMD_CHUNK: handleChunk(cmd.data, cmd.length); break;
        case OTA_CMD_FINISH: handleFinish(); break;
        case OTA_CMD_COMMAND: handleCommand(cmd.json); break;
        default: break;
    }
}

void OtaManager::handleStart(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        status("ERROR", -1, "invalid start json");
        return;
    }

    expectedSize_ = doc["size"] | 0UL;
    const char* crc = doc["crc32"] | "";
    version_ = String((const char*)(doc["version"] | ""));

    int firstDot = version_.indexOf('.');
    int secondDot = version_.indexOf('.', firstDot + 1);

    if (firstDot <= 0 || secondDot <= firstDot) {
        status("ERROR", -1, "invalid version");
        return;
    }

    versionMajor_ = version_.substring(0, firstDot).toInt();
    versionMinor_ = version_.substring(firstDot + 1, secondDot).toInt();
    versionPatch_ = version_.substring(secondDot + 1).toInt();

    if (expectedSize_ == 0 || expectedSize_ > OTA_MAX_IMAGE_SIZE) {
        status("ERROR", -1, "invalid image size");
        return;
    }

    expectedCrc_ = strtoul(crc, nullptr, 16);
    metadataOffset_ = doc["metadata_offset"] | 0UL;
    received_ = 0;
    sequence_ = 0;

    if (file_) file_.close();
    LittleFS.remove(STAGING_FILE);
    file_ = LittleFS.open(STAGING_FILE, "w");
    if (!file_) {
        status("ERROR", -1, "staging file open failed");
        return;
    }

    status("READY", 0);
}

void OtaManager::handleChunk(const uint8_t* packet, uint16_t length) {
    if (!file_ || length < 20) {
        status("ERROR", -1, "no active update");
        return;
    }

    if (memcmp(packet, "OTA1", 4) != 0) {
        status("ERROR", -1, "bad chunk magic");
        return;
    }

    uint32_t seq, offset, payloadCrc;
    uint16_t payloadLen;
    memcpy(&seq, packet + 4, 4);
    memcpy(&offset, packet + 8, 4);
    memcpy(&payloadLen, packet + 12, 2);
    memcpy(&payloadCrc, packet + 16, 4);

    if (payloadLen > OTA_CHUNK_SIZE || 20U + payloadLen != length) {
        status("ERROR", -1, "bad chunk length");
        return;
    }
    if (seq != sequence_ || offset != received_) {
        status("ERROR", -1, "unexpected sequence/offset");
        return;
    }

    const uint8_t* payload = packet + 20;
    if (crc32_compute(payload, payloadLen) != payloadCrc) {
        status("ERROR", -1, "chunk crc mismatch");
        return;
    }

    if (file_.write(payload, payloadLen) != payloadLen) {
        status("ERROR", -1, "flash write failed");
        return;
    }

    received_ += payloadLen;
    sequence_++;

    int progress = expectedSize_ ? (int)((100ULL * received_) / expectedSize_) : 0;
    status("CHUNK_ACK", progress);
}

bool OtaManager::verifyImage() {
    if (!file_) return false;

    file_.flush();
    file_.close();

    File f = LittleFS.open(STAGING_FILE, "r");
    if (!f) return false;

    uint8_t buffer[1024];
    uint32_t crc = crc32_init();

    const uint32_t crcFieldStart = metadataOffset_ + 24;
    const uint32_t crcFieldEnd   = crcFieldStart + 4;

    size_t n;
    uint32_t absoluteOffset = 0;

    while ((n = f.read(buffer, sizeof(buffer))) > 0) {

        for (size_t i = 0; i < n; ++i) {
            uint32_t pos = absoluteOffset + static_cast<uint32_t>(i);

            uint8_t byte = buffer[i];

            if (pos >= crcFieldStart && pos < crcFieldEnd) {
                byte = 0;
            }

            crc = crc32_update(crc, &byte, 1);
        }

        absoluteOffset += static_cast<uint32_t>(n);
    }

    f.close();

    crc = crc32_finalize(crc);

    Serial.printf(
        "[OTA] Expected CRC: %08lX, received CRC: %08lX\n",
        (unsigned long)expectedCrc_,
        (unsigned long)crc
    );

    return received_ == expectedSize_ && crc == expectedCrc_;
}

void OtaManager::handleFinish() {
    if (received_ != expectedSize_) {
        status("ERROR", -1, "size mismatch");
        return;
    }

    status("VERIFYING_IMAGE");
    if (!verifyImage()) {
        status("ERROR", -1, "image crc mismatch");
        return;
    }
    status("IMAGE_READY", 100);
}

bool OtaManager::FlashStagedFirmware(
    File& file,
    uint32_t imageSize,
    uint32_t imageCrc,
    uint32_t metadataOffset,
    uint16_t versionMajor,
    uint16_t versionMinor,
    uint16_t versionPatch)
{
    if (!file) {
        file = LittleFS.open(STAGING_FILE, "r");
    }

    if (!file) {
        Serial.println("[STM32] Cannot open staged firmware");
        return false;
    }

    if (!stm32EnterBootloader()) {
        Serial.println("[STM32] Bootloader not detected");
        return false;
    }

    if (!stm32SendStart(
            imageSize,
            imageCrc,
            metadataOffset,
            versionMajor,
            versionMinor,
            versionPatch)) {
        Serial.println("[STM32] START_UPDATE failed");
        return false;
    }

    file.seek(0);

    uint8_t chunk[OTA_CHUNK_SIZE];
    uint32_t offset = 0;
    uint32_t seq = 0;

    while (offset < imageSize) {
        uint32_t remaining = imageSize - offset;
        uint16_t length = (remaining > OTA_CHUNK_SIZE)
            ? OTA_CHUNK_SIZE
            : static_cast<uint16_t>(remaining);

        size_t n = file.read(chunk, length);
        if (n != length) {
            Serial.println("[STM32] staging read failed");
            return false;
        }

        if (!stm32SendChunk(seq, offset, chunk, length)) {
            Serial.printf("[STM32] DATA failed seq=%lu offset=%lu\n",
                          (unsigned long)seq,
                          (unsigned long)offset);
            return false;
        }

        offset += length;
        seq++;

        int progress = imageSize
            ? static_cast<int>((100ULL * offset) / imageSize)
            : 0;
        status("STM32_FLASHING", progress);
    }

    if (!stm32SendFinish(imageSize, imageCrc)) {
        Serial.println("[STM32] FINISH/CRC verification failed");
        return false;
    }

    status("STM32_VERIFIED", 100);

    if (!stm32Execute(expectedSize_)) {
        Serial.println("[STM32] EXECUTE failed");
        return false;
    }

    return true;
}

void OtaManager::handleCommand(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        status("ERROR", -1, "invalid command json");
        return;
    }

    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "flash") == 0) {

        /*
         * Which STM32 to flash: {"cmd":"flash","board":"stm32-02"}
         * or the 1-based position {"cmd":"flash","board":2}.
         * Without "board", the previously selected one is used
         * (the first board in config.h after boot).
         */
        JsonVariant boardField = doc["board"];

        if (!boardField.isNull()) {
            bool found = boardField.is<const char*>()
                ? stm32SelectBoardById(boardField.as<const char*>())
                : stm32SelectBoardByIndex(boardField.as<int>() - 1);

            if (!found) {
                status("ERROR", -1, "unknown board");
                return;
            }
        }

        status("ENTERING_BOOTLOADER", 100,
               "resetting STM32 and waiting for bootloader");

        File staged = LittleFS.open(STAGING_FILE, "r");
        if (!staged) {
            status("ERROR", -1, "staged firmware not found");
            return;
        }

        bool ok = FlashStagedFirmware(
            staged,
            expectedSize_,
            expectedCrc_,
            metadataOffset_,
            versionMajor_,
            versionMinor_,
            versionPatch_
        );

        staged.close();

        if (!ok) {
            status("ERROR", -1, "STM32 firmware update failed");
            return;
        }

        status("STM32_UPDATE_COMPLETE", 100,
               "STM32 application executed");
    }
    else if (strcmp(cmd, "abort") == 0) {
        if (file_) file_.close();
        stm32Abort();
        status("ABORTED");
    }
}