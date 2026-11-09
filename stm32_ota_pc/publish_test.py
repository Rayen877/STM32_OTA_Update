import json
import paho.mqtt.client as mqtt

BROKER = "127.0.0.1"
PORT = 1883

TOPIC_START = "ota/stm32-01/start"

metadata = {
    "version": "1.2.3",
    "hw_id": "STM32F401RE",
    "size": 1024,
    "crc32": "A83F21C7",
    "chunk_size": 1024,
    "build_id": "20260916-001"
}

client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="pc-ota-test"
)

print("Connecting...")
client.connect(BROKER, PORT, 60)

client.loop_start()

payload = json.dumps(metadata)

print("Publishing:")
print(payload)

client.publish(
    TOPIC_START,
    payload,
    qos=1
)

input("Press Enter to exit...")

client.loop_stop()
client.disconnect()