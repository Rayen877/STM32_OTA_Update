#pragma once

#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define MQTT_HOST       "YOUR_MQTT_BROKER_HOST_IP"
#define MQTT_PORT       1883

#define DEVICE_ID       "stm32-01"

#define TOPIC_ROOT      "ota/stm32-01"
#define TOPIC_START     TOPIC_ROOT "/start"
#define TOPIC_CHUNK     TOPIC_ROOT "/chunk"
#define TOPIC_FINISH    TOPIC_ROOT "/finish"
#define TOPIC_COMMAND   TOPIC_ROOT "/command"
#define TOPIC_STATUS    TOPIC_ROOT "/status"

#define OTA_MAX_IMAGE_SIZE       (512UL * 1024UL)
#define OTA_CHUNK_SIZE           1024
#define OTA_MQTT_PACKET_MAX      (OTA_CHUNK_SIZE + 32)
#define OTA_QUEUE_LENGTH         8

#define WIFI_TIMEOUT_MS          15000UL
#define MQTT_RECONNECT_MS        3000UL

/* ---------------- STM32 over I2C ---------------- */

#define STM32_I2C_SDA_PIN        21
#define STM32_I2C_SCL_PIN        22
#define STM32_I2C_FREQ           100000UL

/*
 * These two MUST match protocol.c on the STM32
 * (BL_I2C_RX_CHUNK and BL_I2C_RESP_PAYLOAD_MAX).
 */
#define STM32_I2C_TX_CHUNK           64
#define STM32_I2C_RESP_PAYLOAD_MAX   48

/*
 * The STM32 erases the whole application area before it answers
 * START_UPDATE, and its CPU cannot serve I2C during the erase.
 * The ESP32 waits this long before it starts reading the answer.
 */
#define STM32_I2C_ERASE_WAIT_MS  8000UL

/*
 * One line per STM32 on the bus.
 *
 *   id        name the dashboard uses ("board" field of the flash command)
 *   i2cAddr   7-bit address = BOARD_I2C_ADDR_7BIT in that board's board_config.h
 *   resetPin  ESP32 GPIO wired to that board's NRST (-1 = not wired)
 */
struct Stm32Board {
    const char* id;
    uint8_t     i2cAddr;
    int8_t      resetPin;
};

static const Stm32Board STM32_BOARDS[] = {
    { "stm32-01", 0x12, 26 },
    { "stm32-02", 0x13, 27 },   /* edit to match your wiring */
};

#define STM32_BOARD_COUNT  (sizeof(STM32_BOARDS) / sizeof(STM32_BOARDS[0]))