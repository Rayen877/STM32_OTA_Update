#ifndef FIRMWARE_METADATA_H
#define FIRMWARE_METADATA_H

#include <stdint.h>

#define FW_METADATA_MAGIC          0x46574D44UL
#define FW_METADATA_FORMAT_VERSION 1U
#define FW_METADATA_HEADER_SIZE    44U
#define FW_HARDWARE_ID             0x31303446UL

/* Exactly 44 bytes, packed to match the PC/ESP32 layout. */
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t hardware_id;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
    uint16_t reserved;
    uint32_t image_size;
    uint32_t image_crc32;
    char build_id[16];
} FirmwareMetadata;

#define FW_METADATA_SIZE       44U
#define FW_METADATA_CRC_OFFSET 24U

#endif
