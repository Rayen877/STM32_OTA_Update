# STM32 OTA PC Dashboard

PC Dashboard -> Paho MQTT -> Mosquitto -> ESP32 LittleFS staging.

## Install
PowerShell:
```powershell
cd C:\stm32_ota_pc
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

Edit `config.py` if needed. Default broker is `127.0.0.1:1883`, device `stm32-01`, application base `0x08010000`.

## Run
```powershell
uvicorn app:app --host 127.0.0.1 --port 8000
```
Open http://127.0.0.1:8000

The app reads `.firmware_info` from the matching ELF, verifies that the section exists in the BIN, patches version/build ID/image size, computes CRC-32/ISO-HDLC with the CRC field zeroed, sends START, sends 1024-byte OTA1 frames, waits for CHUNK_ACK, sends FINISH, then waits for IMAGE_READY.

OTA1 header: `<4sIIHHI` = magic `OTA1`, sequence, firmware offset, payload length, reserved, payload CRC32.

The STM32 UART bootloader is not implemented by this PC package yet.
