#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

uint32_t CRC32_Calculate(
    const uint8_t *data,
    uint32_t length
);

uint32_t CRC32_CalculateWithZeroRange(
    const uint8_t *data,
    uint32_t length,
    uint32_t zero_offset,
    uint32_t zero_length
);

#endif
