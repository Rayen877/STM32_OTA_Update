#include "crc32.h"

#define CRC32_INIT 0xFFFFFFFFUL
#define CRC32_XOR  0xFFFFFFFFUL
#define CRC32_POLY 0xEDB88320UL

static uint32_t CRC32_UpdateByte(
    uint32_t crc,
    uint8_t byte)
{
    crc ^= byte;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        if (crc & 1U)
        {
            crc = (crc >> 1U) ^ CRC32_POLY;
        }
        else
        {
            crc >>= 1U;
        }
    }

    return crc;
}

uint32_t CRC32_Calculate(
    const uint8_t *data,
    uint32_t length)
{
    uint32_t crc = CRC32_INIT;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc = CRC32_UpdateByte(crc, data[i]);
    }

    return crc ^ CRC32_XOR;
}

uint32_t CRC32_CalculateWithZeroRange(
    const uint8_t *data,
    uint32_t length,
    uint32_t zero_offset,
    uint32_t zero_length)
{
    uint32_t crc = CRC32_INIT;

    for (uint32_t i = 0U; i < length; i++)
    {
        uint8_t byte = data[i];

        if ((i >= zero_offset) &&
            (i < zero_offset + zero_length))
        {
            byte = 0U;
        }

        crc = CRC32_UpdateByte(crc, byte);
    }

    return crc ^ CRC32_XOR;
}
