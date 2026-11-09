import paho.mqtt.client as mqtt

BROKER = "127.0.0.1"
PORT = 1883

TOPIC_STATUS = "ota/stm32-01/status"


def on_connect(client, userdata, flags, reason_code, properties=None):
    print("Connected to MQTT broker")
    print("Reason code:", reason_code)

    client.subscribe(TOPIC_STATUS)
    print("Subscribed to:", TOPIC_STATUS)


def on_message(client, userdata, msg):
    print(f"[MQTT] {msg.topic}: {msg.payload.decode(errors='replace')}")


client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="pc-ota-test"
)

client.on_connect = on_connect
client.on_message = on_message

print("Connecting to broker...")
client.connect(BROKER, PORT, 60)

client.loop_forever()