
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ctype.h>
#include <string.h>

// =====================================================
// 1. WIFI AND MQTT CONFIGURATION
// =====================================================

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// =====================================================
// 2. HARDWARE CONFIGURATION
// =====================================================

#define MQ2_PIN       34
#define DHT_PIN       15
#define DHT_TYPE      DHT22

#define RELAY_FAN     27
#define RELAY_PUMP    26

#define LED_GREEN     14
#define LED_RED       12

#define RELAY_ON      LOW
#define RELAY_OFF     HIGH

// =====================================================
// 3. ALARM THRESHOLDS
// =====================================================

const int GAS_THRESHOLD = 1800;
const int GAS_RESET = 1600;

const float TEMP_THRESHOLD = 50.0;
const float TEMP_RESET = 47.0;

const uint32_t SAMPLE_PERIOD_MS = 2000;

// =====================================================
// 4. MQTT TOPICS
// =====================================================

const char* TOPIC_GAS =
    "home/fire/gas";

const char* TOPIC_TEMP =
    "home/fire/temperature";

const char* TOPIC_ALARM =
    "home/fire/alarm";

const char* TOPIC_FAN_SET =
    "home/fire/fan/set";

const char* TOPIC_PUMP_SET =
    "home/fire/pump/set";

const char* TOPIC_MODE_SET =
    "home/fire/mode/set";

const char* TOPIC_FAN_STATE =
    "home/fire/fan/state";

const char* TOPIC_PUMP_STATE =
    "home/fire/pump/state";

const char* TOPIC_MODE_STATE =
    "home/fire/mode/state";

// =====================================================
// 5. DATA STRUCTURES
// =====================================================

struct SensorData {
    int gas;
    float temperature;
    bool temperatureValid;
};

enum CommandType {
    CMD_FAN,
    CMD_PUMP,
    CMD_MODE
};

struct ControlCommand {
    CommandType type;
    bool value;
};

struct SystemState {
    bool gasAlarm;
    bool tempAlarm;
    bool manualMode;
    bool fanOutput;
    bool pumpOutput;
};

QueueHandle_t dataQueue = nullptr;
QueueHandle_t alertQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t stateQueue = nullptr;

// =====================================================
// 6. WIFI, MQTT AND SENSOR OBJECTS
// =====================================================

WiFiClient wifiClient;

PubSubClient mqtt(wifiClient);

DHT dht(DHT_PIN, DHT_TYPE);

// =====================================================
// 7. GLOBAL STATE
// =====================================================

bool sameState(const SystemState& a,
               const SystemState& b) {

    return a.gasAlarm == b.gasAlarm &&
           a.tempAlarm == b.tempAlarm &&
           a.manualMode == b.manualMode &&
           a.fanOutput == b.fanOutput &&
           a.pumpOutput == b.pumpOutput;
}

// =====================================================
// 8. MQTT PUBLISH FUNCTIONS
// =====================================================

void publishText(const char* topic,
                 const char* value) {

    if (mqtt.connected()) {
        mqtt.publish(topic, value, true);
    }
}

void publishStates(const SystemState& state) {

    publishText(
        TOPIC_FAN_STATE,
        state.fanOutput ? "ON" : "OFF"
    );

    publishText(
        TOPIC_PUMP_STATE,
        state.pumpOutput ? "ON" : "OFF"
    );

    publishText(
        TOPIC_MODE_STATE,
        state.manualMode ? "MANUAL" : "AUTO"
    );

    publishText(
        TOPIC_ALARM,
        (state.gasAlarm || state.tempAlarm)
        ? "ALARM" : "NORMAL"
    );
}

// =====================================================
// 9. MQTT CALLBACK
// =====================================================

void mqttCallback(char* topic,
                  byte* payload,
                  unsigned int length) {

    char message[16];

    if (length == 0 || length >= sizeof(message)) {
        return;
    }

    for (unsigned int i = 0; i < length; i++) {
        message[i] = (char)payload[i];
    }

    message[length] = '\0';

    for (unsigned int i = 0; i < length; i++) {
        message[i] = toupper(
            (unsigned char)message[i]
        );
    }

    ControlCommand cmd;
    bool valid = false;

    if (strcmp(topic, TOPIC_FAN_SET) == 0) {

        if (strcmp(message, "ON") == 0 ||
            strcmp(message, "OFF") == 0) {

            cmd.type = CMD_FAN;
            cmd.value = (strcmp(message, "ON") == 0);
            valid = true;
        }

    } else if (strcmp(topic, TOPIC_PUMP_SET) == 0) {

        if (strcmp(message, "ON") == 0 ||
            strcmp(message, "OFF") == 0) {

            cmd.type = CMD_PUMP;
            cmd.value = (strcmp(message, "ON") == 0);
            valid = true;
        }

    } else if (strcmp(topic, TOPIC_MODE_SET) == 0) {

        if (strcmp(message, "AUTO") == 0 ||
            strcmp(message, "MANUAL") == 0) {

            cmd.type = CMD_MODE;
            cmd.value = (strcmp(message, "MANUAL") == 0);
            valid = true;
        }
    }

    // Callback chỉ tiếp nhận lệnh.
    // Không trực tiếp điều khiển relay.

    if (valid && commandQueue != nullptr) {

        if (xQueueSend(commandQueue, &cmd, 0) != pdPASS) {

            Serial.println(
                "Canh bao: commandQueue day!"
            );
        }
    }
}

// =====================================================
// 10. WIFI CONNECTION
// =====================================================

bool connectWiFi() {

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Dang ket noi Wi-Fi");

    uint32_t started = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 20000) {

        vTaskDelay(pdMS_TO_TICKS(500));

        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {

        Serial.println("\nWi-Fi ket noi thanh cong!");

        Serial.print("IP: ");
        Serial.println(WiFi.localIP());

        return true;
    }

    Serial.println("\nWi-Fi ket noi that bai!");

    return false;
}

// =====================================================
// 11. MQTT CONNECTION
// =====================================================

bool connectMQTT() {

    if (mqtt.connected()) {
        return true;
    }

    String clientId =
        "ESP32-Fire-" +
        String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("Dang ket noi MQTT...");

    if (mqtt.connect(clientId.c_str())) {

        mqtt.subscribe(TOPIC_FAN_SET);
        mqtt.subscribe(TOPIC_PUMP_SET);
        mqtt.subscribe(TOPIC_MODE_SET);

        Serial.println(" MQTT OK!");

        return true;
    }

    Serial.printf(
        " MQTT that bai, state = %d\n",
        mqtt.state()
    );

    return false;
}

// =====================================================
// 12. TASK 1: READ SENSOR DATA
// CORE 1
// =====================================================

void taskReadData(void* parameter) {

    (void)parameter;

    for (;;) {

        SensorData sample;

        // Đọc cảm biến MQ-2.
        sample.gas = analogRead(MQ2_PIN);

        // Đọc nhiệt độ DHT22.
        sample.temperature = dht.readTemperature();

        sample.temperatureValid =
            !isnan(sample.temperature);

        // Gửi mẫu dữ liệu đến các Task.
        xQueueOverwrite(dataQueue, &sample);

        xQueueOverwrite(alertQueue, &sample);

        vTaskDelay(
            pdMS_TO_TICKS(SAMPLE_PERIOD_MS)
        );
    }
}

// =====================================================
// 13. TASK 2: ALARM AND DEVICE CONTROL
// CORE 1
// =====================================================

void taskAlert(void* parameter) {

    (void)parameter;

    SensorData sample;
    ControlCommand cmd;

    bool gasAlarm = false;
    bool tempAlarm = false;

    bool manualMode = false;
    bool manualFan = false;
    bool manualPump = false;

    bool fanOutput = false;
    bool pumpOutput = false;

    SystemState previousState = {
        false, false, false, false, false
    };

    bool firstUpdate = true;

    for (;;) {

        bool newSample = false;

        // ---------------------------------------------
        // A. PROCESS MQTT COMMANDS
        // ---------------------------------------------

        while (
            xQueueReceive(commandQueue, &cmd, 0)
            == pdPASS
        ) {

            if (cmd.type == CMD_MODE) {

                manualMode = cmd.value;

            } else if (cmd.type == CMD_FAN) {

                manualFan = cmd.value;

            } else if (cmd.type == CMD_PUMP) {

                manualPump = cmd.value;
            }
        }

        // ---------------------------------------------
        // B. RECEIVE SENSOR DATA
        // ---------------------------------------------

        if (
            xQueueReceive(
                alertQueue,
                &sample,
                pdMS_TO_TICKS(20)
            ) == pdPASS
        ) {

            newSample = true;

            // Gas alarm with hysteresis.
            if (sample.gas >= GAS_THRESHOLD) {

                gasAlarm = true;

            } else if (sample.gas <= GAS_RESET) {

                gasAlarm = false;
            }

            // Temperature alarm with hysteresis.
            if (sample.temperatureValid) {

                if (
                    sample.temperature >= TEMP_THRESHOLD
                ) {

                    tempAlarm = true;

                } else if (
                    sample.temperature <= TEMP_RESET
                ) {

                    tempAlarm = false;
                }
            }
        }

        // ---------------------------------------------
        // C. CALCULATE DEVICE OUTPUTS
        // ---------------------------------------------

        if (manualMode) {

            fanOutput = manualFan || gasAlarm;

            pumpOutput = manualPump || tempAlarm;

        } else {

            fanOutput = gasAlarm;

            pumpOutput = tempAlarm;
        }

        // ---------------------------------------------
        // D. CONTROL RELAYS
        // ---------------------------------------------

        digitalWrite(
            RELAY_FAN,
            fanOutput ? RELAY_ON : RELAY_OFF
        );

        digitalWrite(
            RELAY_PUMP,
            pumpOutput ? RELAY_ON : RELAY_OFF
        );

        // ---------------------------------------------
        // E. CONTROL STATUS LEDS
        // ---------------------------------------------

        bool alarmActive = gasAlarm || tempAlarm;

        digitalWrite(
            LED_GREEN,
            alarmActive ? LOW : HIGH
        );

        digitalWrite(
            LED_RED,
            alarmActive ? HIGH : LOW
        );

        // ---------------------------------------------
        // F. DISPLAY ALL INFORMATION ON ONE LINE
        // ---------------------------------------------

        if (newSample) {

            Serial.printf(
                "MQ2 ADC: %d | Nhiet do: ",
                sample.gas
            );

            if (sample.temperatureValid) {

                Serial.printf(
                    "%.2f C",
                    sample.temperature
                );

            } else {

                Serial.print("LOI DHT22");
            }

            Serial.printf(
                " | Che do: %s | QUAT: %s | BOM: %s\n",
                manualMode ? "MANUAL" : "AUTO",
                fanOutput ? "ON" : "OFF",
                pumpOutput ? "ON" : "OFF"
            );
        }

        // ---------------------------------------------
        // G. SEND SYSTEM STATE TO MQTT TASK
        // ---------------------------------------------

        SystemState currentState = {
            gasAlarm,
            tempAlarm,
            manualMode,
            fanOutput,
            pumpOutput
        };

        if (
            firstUpdate ||
            !sameState(currentState, previousState)
        ) {

            xQueueOverwrite(
                stateQueue,
                &currentState
            );

            previousState = currentState;

            firstUpdate = false;
        }
    }
}

// =====================================================
// 14. TASK 3: WIFI AND MQTT TRANSMISSION
// CORE 0
// =====================================================

void taskTransmitData(void* parameter) {

    (void)parameter;

    SensorData sample;

    SystemState state;

    for (;;) {

        // ---------------------------------------------
        // A. CHECK WIFI
        // ---------------------------------------------

        if (WiFi.status() != WL_CONNECTED) {

            if (!connectWiFi()) {

                vTaskDelay(pdMS_TO_TICKS(2000));

                continue;
            }
        }

        // ---------------------------------------------
        // B. CHECK MQTT
        // ---------------------------------------------

        if (!mqtt.connected()) {

            if (!connectMQTT()) {

                vTaskDelay(pdMS_TO_TICKS(2000));

                continue;
            }

            // Publish trạng thái hiện tại sau reconnect.
            if (
                xQueuePeek(stateQueue, &state, 0) == pdPASS
            ) {

                publishStates(state);
            }
        }

        // ---------------------------------------------
        // C. MAINTAIN MQTT CONNECTION
        // ---------------------------------------------

        mqtt.loop();

        // ---------------------------------------------
        // D. PUBLISH SENSOR DATA
        // ---------------------------------------------

        if (
            xQueueReceive(dataQueue, &sample, 0) == pdPASS
        ) {

            char gasBuffer[16];
            char tempBuffer[20];

            snprintf(
                gasBuffer,
                sizeof(gasBuffer),
                "%d",
                sample.gas
            );

            publishText(
                TOPIC_GAS,
                gasBuffer
            );

            if (sample.temperatureValid) {

                dtostrf(
                    sample.temperature,
                    1,
                    2,
                    tempBuffer
                );

                publishText(
                    TOPIC_TEMP,
                    tempBuffer
                );

            } else {

                publishText(
                    TOPIC_TEMP,
                    "ERROR"
                );
            }
        }

        // ---------------------------------------------
        // E. PUBLISH SYSTEM STATE
        // ---------------------------------------------

        if (
            xQueueReceive(stateQueue, &state, 0) == pdPASS
        ) {

            publishStates(state);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =====================================================
// 15. SETUP
// =====================================================

void setup() {

    Serial.begin(115200);

    analogReadResolution(12);

    pinMode(MQ2_PIN, INPUT);

    pinMode(RELAY_FAN, OUTPUT);
    pinMode(RELAY_PUMP, OUTPUT);

    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_RED, OUTPUT);

    // Initial device states.
    digitalWrite(RELAY_FAN, RELAY_OFF);
    digitalWrite(RELAY_PUMP, RELAY_OFF);

    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);

    dht.begin();

    // MQTT configuration.
    mqtt.setServer(MQTT_HOST, MQTT_PORT);

    mqtt.setCallback(mqttCallback);

    mqtt.setBufferSize(512);

    mqtt.setKeepAlive(30);

    mqtt.setSocketTimeout(5);

    // ---------------------------------------------
    // CREATE FREERTOS QUEUES
    // ---------------------------------------------

    dataQueue = xQueueCreate(
        1,
        sizeof(SensorData)
    );

    alertQueue = xQueueCreate(
        1,
        sizeof(SensorData)
    );

    commandQueue = xQueueCreate(
        8,
        sizeof(ControlCommand)
    );

    stateQueue = xQueueCreate(
        1,
        sizeof(SystemState)
    );

    if (
        !dataQueue ||
        !alertQueue ||
        !commandQueue ||
        !stateQueue
    ) {

        Serial.println(
            "Loi cap phat Queue! Dung he thong."
        );

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // ---------------------------------------------
    // CREATE FREERTOS TASKS
    // ---------------------------------------------

    // TASK 3: MQTT - CORE 0
    xTaskCreatePinnedToCore(
        taskTransmitData,
        "TaskTruyenData",
        8192,
        nullptr,
        2,
        nullptr,
        0
    );

    // TASK 1: SENSOR - CORE 1
    xTaskCreatePinnedToCore(
        taskReadData,
        "TaskDocData",
        4096,
        nullptr,
        2,
        nullptr,
        1
    );

    // TASK 2: ALARM - CORE 1
    xTaskCreatePinnedToCore(
        taskAlert,
        "TaskCanhBao",
        4096,
        nullptr,
        3,
        nullptr,
        1
    );

    Serial.println(
        "HE THONG BAO CHAY ESP32 KHOI DONG!"
    );
}

// =====================================================
// 16. LOOP
// =====================================================

void loop() {

    // Main processing is handled by FreeRTOS tasks.

    vTaskDelay(pdMS_TO_TICKS(1000));
}
