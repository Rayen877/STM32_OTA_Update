#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define OTA_STATUS_MAX 384

struct OtaStatusMessage {
    char json[OTA_STATUS_MAX];
};

enum OtaCommandType {
    OTA_CMD_START,
    OTA_CMD_CHUNK,
    OTA_CMD_FINISH,
    OTA_CMD_COMMAND
};

struct OtaCommand {
    OtaCommandType type;
    char json[600];
    uint8_t* data;
    uint16_t length;
};

class OtaManager {
public:
    void begin(QueueHandle_t statusQueue);
    void handle(const OtaCommand& cmd);

private:
    QueueHandle_t statusQueue_ = nullptr;
    File file_;
    uint32_t expectedSize_ = 0;
    uint32_t expectedCrc_ = 0;
    uint32_t metadataOffset_ = 0;
    uint32_t received_ = 0;
    uint32_t sequence_ = 0;

    uint16_t versionMajor_ = 0;
    uint16_t versionMinor_ = 0;
    uint16_t versionPatch_ = 0;
    String version_;

    void status(const char* state, int progress = -1, const char* detail = nullptr);
    void handleStart(const char* json);
    void handleChunk(const uint8_t* packet, uint16_t length);
    void handleFinish();
    void handleCommand(const char* json);
    bool verifyImage();
    bool FlashStagedFirmware(
        File& file,
        uint32_t imageSize,
        uint32_t imageCrc,
        uint32_t metadataOffset,
        uint16_t versionMajor,
        uint16_t versionMinor,
        uint16_t versionPatch
    );
};
