#include "flash_if.h"

#include <string.h>

HAL_StatusTypeDef FlashIF_EraseApplication(void)
{
    FLASH_EraseInitTypeDef erase = {0};

    uint32_t sector_error = 0U;

    HAL_StatusTypeDef status =
        HAL_FLASH_Unlock();

    if (status != HAL_OK)
    {
        return status;
    }

    /*
     * F401RE:
     *
     * Sector 4 = 0x08010000
     * Sector 5 = 0x08020000
     * Sector 6 = 0x08040000
     * Sector 7 = 0x08060000
     *
     * These four sectors contain the entire
     * 448 KB application region.
     */
    erase.TypeErase =
        FLASH_TYPEERASE_SECTORS;

    erase.Sector =
        FLASH_SECTOR_4;

    erase.NbSectors =
        4U;

    erase.VoltageRange =
        FLASH_VOLTAGE_RANGE_3;

    status =
        HAL_FLASHEx_Erase(
            &erase,
            &sector_error
        );

    HAL_FLASH_Lock();

    return status;
}

HAL_StatusTypeDef FlashIF_Write(
    uint32_t address,
    const uint8_t *data,
    uint32_t length)
{
    HAL_StatusTypeDef status =
        HAL_FLASH_Unlock();

    if (status != HAL_OK)
    {
        return status;
    }

    for (uint32_t offset = 0U;
         offset < length;
         offset += 4U)
    {
        uint32_t word =
            0xFFFFFFFFUL;

        uint32_t remaining =
            length - offset;

        if (remaining >= 4U)
        {
            memcpy(
                &word,
                &data[offset],
                4U
            );
        }
        else
        {
            memcpy(
                &word,
                &data[offset],
                remaining
            );
        }

        status =
            HAL_FLASH_Program(
                FLASH_TYPEPROGRAM_WORD,
                address + offset,
                word
            );

        if (status != HAL_OK)
        {
            break;
        }
    }

    HAL_FLASH_Lock();

    return status;
}
