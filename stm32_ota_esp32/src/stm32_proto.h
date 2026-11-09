#pragma once

#include <Arduino.h>

void stm32ProtocolBegin();

/* Choose which STM32 the next calls talk to (default: first board in config.h). */
bool stm32SelectBoardById(const char* id);
bool stm32SelectBoardByIndex(int index);      /* 0-based */
const char* stm32ActiveBoardId();

bool stm32EnterBootloader();

bool stm32SendStart(
    uint32_t imageSize,
    uint32_t imageCrc,
    uint32_t metadataOffset,
    uint16_t versionMajor,
    uint16_t versionMinor,
    uint16_t versionPatch
);

bool stm32SendChunk(
    uint32_t sequence,
    uint32_t offset,
    const uint8_t* data,
    uint16_t length
);

bool stm32SendFinish(
    uint32_t imageSize,
    uint32_t imageCrc
);

bool stm32Execute(uint32_t imageSize);

void stm32Abort();