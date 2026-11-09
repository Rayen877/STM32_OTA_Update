#include "firmware_metadata.h"

__attribute__((section(".firmware_info"), used, aligned(4)))
const FirmwareMetadata g_firmware_metadata =
{
    .magic = FW_METADATA_MAGIC,
    .format_version = FW_METADATA_FORMAT_VERSION,
    .header_size = FW_METADATA_HEADER_SIZE,
    .hardware_id = FW_HARDWARE_ID,
    .version_major = 1,
    .version_minor = 0,
    .version_patch = 0,
    .reserved = 0,
    .image_size = 0,
    .image_crc32 = 0,
    .build_id = "DEV-0001"
};
