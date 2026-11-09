#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define BOOTLOADER_VERSION_MAJOR  1U
#define BOOTLOADER_VERSION_MINOR  0U
#define BOOTLOADER_VERSION_PATCH  0U

#define APP_ADDRESS       0x08010000UL
#define APP_MAX_SIZE      (448UL * 1024UL)
#define APP_END_ADDRESS   (APP_ADDRESS + APP_MAX_SIZE)

/*
 * "F401" in little-endian memory.
 */
#define APP_HARDWARE_ID        0x31303446UL
#define APP_HARDWARE_ID_STRING "STM32F401RE"

/*
 * Keep disabled during the first implementation.
 * Set to 1 later to reject versions <= current version.
 */
#define BL_ENFORCE_NEWER_VERSION 1U

#define BOOT_WAIT_MS 4000U

void Bootloader_Run(I2C_HandleTypeDef *hi2c);

bool Bootloader_ApplicationIsValid(void);

void Bootloader_JumpToApplication(void);

#endif
