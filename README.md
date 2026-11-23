
# STM32 Firmware OTA Update System

An Over-The-Air (OTA) firmware update system for multiple STM32 microcontrollers, using an ESP32 as an intermediary between the STM32 devices and a PC-based Python dashboard.

The system allows firmware to be uploaded remotely through MQTT, distributed by the ESP32 over an I2C bus, and installed using a custom bootloader running on each STM32.

## Key Features

- **Multi-device support:** Update multiple STM32 boards through a shared I2C bus, with each device identified by a unique ID.
- **OTA firmware updates:** Transfer and install new firmware without requiring direct access to the STM32 programming interface.
- **Custom bootloader:** Each STM32 runs a bootloader responsible for receiving and installing firmware updates.
- **Firmware version management:** Each device maintains its firmware version, preventing updates with older firmware.
- **CRC verification:** Data integrity is checked at both communication stages:
  - PC ↔ ESP32
  - ESP32 ↔ STM32
- **MQTT communication:** The ESP32 communicates with the PC dashboard through MQTT.
- **Python dashboard:** Provides an interface to manage devices and initiate firmware updates by uploading the project's ELF and BIN files.

## System Architecture

```text
       PC / Python Dashboard
                |
               MQTT
                |
              ESP32
          (I2C Master)
                |
        I2C Communication
          /     |     \
       STM32  STM32  STM32
        #1     #2     #3
       Bootloader on each device
```

## Firmware Update Workflow

1. Upload the ELF and BIN firmware files through the Python dashboard.
2. The ESP32 receives the firmware through MQTT.
3. The target STM32 is identified using its unique device ID.
4. The current firmware version is checked to prevent downgrades.
5. Firmware data is transferred over I2C with CRC verification.
6. The STM32 bootloader verifies and installs the new firmware.
7. The device restarts and runs the updated application.

## Technologies Used

- **Microcontrollers:** STM32F401RE, ESP32
- **Programming:** C / C++, Python
- **Communication:** I2C, MQTT
- **Firmware Management:** Custom STM32 Bootloader, CRC
- **Dashboard:** Python
