#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "crc32.h"
#include "ota_manager.h"
#include "stm32_proto.h"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

static TaskHandle_t otaTaskHandle = nullptr;
static QueueHandle_t otaQueue = nullptr;
static QueueHandle_t statusQueue = nullptr;

static void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Connecting");

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WiFi] Connection failed");
    }
}

static void publishStatus(const char* state, int progress = -1, const char* detail = nullptr) {
    JsonDocument doc;
    doc["state"] = state;
    if (progress >= 0) doc["progress"] = progress;
    if (detail) doc["detail"] = detail;

    String json;
    serializeJson(doc, json);
    mqttClient.publish(TOPIC_STATUS, json.c_str(), true);
    Serial.print("[OTA] ");
    Serial.println(json);
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] topic=%s len=%u\n", topic, length);

    if (strcmp(topic, TOPIC_START) == 0) {
        String message((char*)payload, length);
        OtaCommand cmd{};
        cmd.type = OTA_CMD_START;
        message.toCharArray(cmd.json, sizeof(cmd.json));
        xQueueSend(otaQueue, &cmd, pdMS_TO_TICKS(100));
        return;
    }

    if (strcmp(topic, TOPIC_CHUNK) == 0) {
        // Binary packet is copied to heap because MQTT payload is only valid in callback.
        OtaCommand cmd{};
        cmd.type = OTA_CMD_CHUNK;
        cmd.length = min((unsigned int)OTA_MQTT_PACKET_MAX, length);
        cmd.data = (uint8_t*)malloc(cmd.length);
        if (!cmd.data) {
            publishStatus("ERROR", -1, "malloc failed");
            return;
        }
        memcpy(cmd.data, payload, cmd.length);
        if (xQueueSend(otaQueue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
            free(cmd.data);
            publishStatus("ERROR", -1, "OTA queue full");
        }
        return;
    }

    if (strcmp(topic, TOPIC_FINISH) == 0) {
        OtaCommand cmd{};
        cmd.type = OTA_CMD_FINISH;
        xQueueSend(otaQueue, &cmd, pdMS_TO_TICKS(100));
        return;
    }

    if (strcmp(topic, TOPIC_COMMAND) == 0) {
        String message((char*)payload, length);
        OtaCommand cmd{};
        cmd.type = OTA_CMD_COMMAND;
        message.toCharArray(cmd.json, sizeof(cmd.json));
        xQueueSend(otaQueue, &cmd, pdMS_TO_TICKS(100));
    }
}

static void publishQueuedStatuses() {
    if (!mqttClient.connected()) return;

    OtaStatusMessage msg{};
    while (xQueueReceive(statusQueue, &msg, 0) == pdPASS) {
        mqttClient.publish(TOPIC_STATUS, msg.json, true);
    }
}

static void connectMQTT() {
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(OTA_MQTT_PACKET_MAX + 256);

    while (!mqttClient.connected()) {
        String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        Serial.printf("[MQTT] Connecting to %s:%u...\n", MQTT_HOST, MQTT_PORT);

        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("[MQTT] Connected");
            mqttClient.subscribe(TOPIC_START, 1);
            mqttClient.subscribe(TOPIC_CHUNK, 1);
            mqttClient.subscribe(TOPIC_FINISH, 1);
            mqttClient.subscribe(TOPIC_COMMAND, 1);
            publishStatus("IDLE");
        } else {
            Serial.printf("[MQTT] failed rc=%d\n", mqttClient.state());
            delay(MQTT_RECONNECT_MS);
        }
    }
}

static void mqttTask(void*) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) connectWiFi();
        if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) connectMQTT();
        mqttClient.loop();
        publishQueuedStatuses();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void otaTask(void*) {
    OtaManager manager;
    manager.begin(statusQueue);

    for (;;) {
        OtaCommand cmd{};
        if (xQueueReceive(otaQueue, &cmd, portMAX_DELAY) == pdPASS) {
            manager.handle(cmd);
            if (cmd.type == OTA_CMD_CHUNK && cmd.data) free(cmd.data);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("=== STM32 OTA ESP32 Gateway ===");

    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed");
    } else {
        Serial.println("[FS] LittleFS mounted");
    }
    stm32ProtocolBegin();      /* I2C + NRST pins of every board */

    otaQueue = xQueueCreate(OTA_QUEUE_LENGTH, sizeof(OtaCommand));
    statusQueue = xQueueCreate(OTA_QUEUE_LENGTH, sizeof(OtaStatusMessage));

    if (!otaQueue || !statusQueue) {
        Serial.println("[FATAL] Queue creation failed");
        while (true) delay(1000);
    }

    xTaskCreatePinnedToCore(mqttTask, "mqtt_task", 6144, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(otaTask, "ota_task", 8192, nullptr, 3, &otaTaskHandle, 1);

    connectWiFi();
}

void loop() {
   vTaskDelay(pdMS_TO_TICKS(1000));
}