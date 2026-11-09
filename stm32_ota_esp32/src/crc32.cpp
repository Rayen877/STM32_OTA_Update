#include "crc32.h"

static uint32_t table[256];
static bool tableReady = false;

static void initTable() {
    if (tableReady) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1U) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    tableReady = true;
}

uint32_t crc32_init(void) {
    initTable();
    return 0xFFFFFFFFUL;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length) {
    initTable();
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32_finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t crc32_compute(const uint8_t* data, size_t length) {
    return crc32_finalize(crc32_update(crc32_init(), data, length));
}
