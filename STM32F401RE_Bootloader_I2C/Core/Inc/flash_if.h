#ifndef FLASH_IF_H
#define FLASH_IF_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

HAL_StatusTypeDef FlashIF_EraseApplication(void);

HAL_StatusTypeDef FlashIF_Write(
    uint32_t address,
    const uint8_t *data,
    uint32_t length
);

#endif
