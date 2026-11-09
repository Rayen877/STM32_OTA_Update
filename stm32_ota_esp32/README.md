# STM32 OTA ESP32 Gateway - PlatformIO Arduino

ESP32 gateway for the STM32F401RE OTA project.

## Stack
- PlatformIO
- Arduino framework
- FreeRTOS tasks provided by ESP32 Arduino
- PubSubClient MQTT
- ArduinoJson
- LittleFS for firmware staging
- CRC-32/ISO-HDLC

## Setup
1. Open this folder in VS Code with PlatformIO.
2. Edit `src/config.h`.
3. Set Wi-Fi credentials and the PC IP running Mosquitto.
4. Build with `pio run`.
5. Upload with `pio run -t upload`.
6. Monitor with `pio device monitor`.

The STM32 UART flashing functions are intentionally scaffolded until the STM32 bootloader is implemented. The MQTT download/staging path is functional.
