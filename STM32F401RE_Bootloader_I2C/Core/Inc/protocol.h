#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "stm32f4xx_hal.h"

#include <stdint.h>

/*
 * Wire format:
 *
 * 0-1    magic
 * 2      command
 * 3      flags
 * 4-5    payload length
 * 6-9    sequence
 * 10-13  firmware offset
 * 14-17  payload CRC32
 *
 * Total header = 18 bytes
 */

#define BL_MAGIC        0xA55AU
#define BL_MAX_PAYLOAD 1024U

#define BL_CMD_HELLO         0x01U
#define BL_CMD_GET_INFO      0x02U

#define BL_CMD_START_UPDATE  0x10U
#define BL_CMD_DATA          0x11U
#define BL_CMD_FINISH        0x12U
#define BL_CMD_VERIFY        0x13U
#define BL_CMD_EXECUTE       0x14U

#define BL_CMD_ABORT         0x7FU

#define BL_STATUS_OK              0x00U
#define BL_STATUS_INVALID_COMMAND 0x01U
#define BL_STATUS_BAD_LENGTH      0x02U
#define BL_STATUS_BAD_CRC         0x03U
#define BL_STATUS_BAD_SEQUENCE    0x04U
#define BL_STATUS_FLASH_ERROR     0x05U
#define BL_STATUS_INVALID_IMAGE   0x06U
#define BL_STATUS_VERSION_ERROR   0x07U
#define BL_STATUS_HARDWARE_ERROR  0x08U
#define BL_STATUS_NOT_READY       0x09U

#define BOARD_I2C_ADDR_7BIT  0x12U

typedef enum
{
    PROTOCOL_OK = 0,
    PROTOCOL_TIMEOUT,
    PROTOCOL_BAD_LENGTH,
    PROTOCOL_BAD_CRC

} ProtocolResult;

typedef struct __attribute__((packed))
{
    uint16_t magic;

    uint8_t command;
    uint8_t flags;

    uint16_t payload_length;

    uint32_t sequence;
    uint32_t offset;

    uint32_t payload_crc32;

} ProtocolHeader;

typedef struct
{
    ProtocolHeader header;

    uint8_t payload[BL_MAX_PAYLOAD];

} ProtocolFrame;

typedef struct __attribute__((packed))
{
    uint8_t status;

    uint8_t reserved[3];

    uint32_t sequence;
    uint32_t next_offset;

} AckPayload;

typedef struct __attribute__((packed))
{
    uint32_t image_size;
    uint32_t image_crc32;

    uint32_t metadata_offset;

    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;

    uint16_t reserved;

    char hardware_id[16];
    char build_id[16];

} StartUpdatePayload;

typedef struct __attribute__((packed))
{
    uint8_t status;
    uint8_t app_valid;

    uint16_t boot_major;
    uint16_t boot_minor;
    uint16_t boot_patch;

    uint32_t app_size;
    uint32_t hardware_id;

    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;

    uint16_t reserved;
    uint32_t uid[3];

} InfoResponsePayload;

void Protocol_Init(I2C_HandleTypeDef *hi2c);

ProtocolResult Protocol_Receive(
    ProtocolFrame *frame,
    uint32_t timeout_ms
);

HAL_StatusTypeDef Protocol_Send(
    uint8_t command,
    uint32_t sequence,
    uint32_t offset,
    const uint8_t *payload,
    uint16_t payload_length
);

HAL_StatusTypeDef Protocol_SendAck(
    uint8_t request_command,
    uint8_t status,
    uint32_t sequence,
    uint32_t next_offset
);

#endif
