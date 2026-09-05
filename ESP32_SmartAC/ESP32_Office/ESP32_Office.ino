// 辦公室 ESP32 韌體 - Sampo 聲寶冷氣
// 24/7 + 防快速連點 + MQTT/WiFi 自動恢復 + Watchdog + Reset Log
#define ROOM_OFFICE

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Kelon.h>

// ==================== Wi-Fi ====================
const char* ssid = "XYZ";
const char* password = "1234567890";

// ==================== MQTT ====================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

#ifdef ROOM_OFFICE
const char* mqtt_topic = "office/ac";
#else
const char* mqtt_topic = "home/livingroom/ac";
#endif

// ==================== Hardware ====================
const uint16_t kIrLed = 13;
IRKelonAc ac(kIrLed);
IRsend rawSender(kIrLed);
WiFiClient espClient;
PubSubClient client(espClient);

// ==================== Watchdog ====================
#define WDT_TIMEOUT 15

// ==================== Timing ====================
const unsigned long WIFI_CHECK_INTERVAL = 5000;
const unsigned long WIFI_BEGIN_INTERVAL = 30000;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long WIFI_RESTART_TIMEOUT = 5UL * 60UL * 1000UL;
const unsigned long MQTT_RESTART_TIMEOUT = 10UL * 60UL * 1000UL;
const unsigned long STATUS_REPORT_INTERVAL = 30000;

// 收到指令後等待 350ms；期間新指令會覆蓋舊指令
const unsigned long COMMAND_SETTLE_MS = 350;

// IR 最短發送間隔
const unsigned long IR_MIN_INTERVAL_MS = 300;

// 0 = 不定期重開
const unsigned long REBOOT_INTERVAL_MS = 0;

// ==================== Runtime ====================
unsigned long setupTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastWiFiBeginTime = 0;
unsigned long lastMqttConnectAttempt = 0;
unsigned long lastStatusReportTime = 0;
unsigned long wifiDisconnectedSince = 0;
unsigned long mqttDisconnectedSince = 0;
unsigned long mqttMessageCount = 0;
unsigned long irTransmitCount = 0;
unsigned long lastIrTransmitTime = 0;

// ==================== Pending Queue ====================
// MQTT callback 不直接發 IR，避免 callback 被 IR 傳輸阻塞。
// 採 LAST COMMAND WINS。
struct PendingCommand {
  bool valid;
  char command[20];
  int temperature;
  char traceId[80];
  unsigned long receivedAt;
  unsigned long mqttNumber;
};

PendingCommand pending = {false, "", 25, "", 0, 0};

// ==================== Reset Reason ====================
const char* getResetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:     return "UNKNOWN";
    case ESP_RST_POWERON:     return "POWER_ON";
    case ESP_RST_EXT:         return "EXTERNAL_RESET";
    case ESP_RST_SW:          return "SOFTWARE_RESET";
    case ESP_RST_PANIC:       return "PANIC_EXCEPTION";
    case ESP_RST_INT_WDT:     return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT:    return "TASK_WATCHDOG";
    case ESP_RST_WDT:         return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP:   return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:    return "BROWNOUT";
    case ESP_RST_SDIO:        return "SDIO_RESET";
    default:                  return "UNRECOGNIZED";
  }
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[BOOT] RESET INFORMATION");
  Serial.println("------------------------------------------------------------");
  Serial.print("[BOOT] Reset reason = ");
  Serial.println(getResetReasonName(reason));
  Serial.print("[BOOT] Reset code   = ");
  Serial.println((int)reason);
  Serial.print("[BOOT] Chip model   = ");
  Serial.println(ESP.getChipModel());
  Serial.print("[BOOT] CPU freq     = ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
  Serial.print("[BOOT] Flash size   = ");
  Serial.print(ESP.getFlashChipSize());
  Serial.println(" bytes");
  Serial.print("[BOOT] Free heap    = ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.println("============================================================");
}

// ==================== Watchdog ====================
void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_err_t r = esp_task_wdt_init(&config);
  if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
    Serial.print("[WDT] init failed = ");
    Serial.println((int)r);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
#endif

  esp_err_t addResult = esp_task_wdt_add(NULL);
  if (addResult == ESP_OK || addResult == ESP_ERR_INVALID_STATE) {
    Serial.print("[WDT] ENABLED | Timeout = ");
    Serial.print(WDT_TIMEOUT);
    Serial.println(" sec");
  } else {
    Serial.print("[WDT] add task failed = ");
    Serial.println((int)addResult);
  }
}

inline void feedWatchdog() {
  esp_task_wdt_reset();
}

// ==================== Sampo Code ====================
uint64_t getSampoCode(int temp) {
  if (temp < 16) temp = 16;
  if (temp > 30) temp = 30;

  uint8_t high = (uint8_t)(temp - 16);
  uint8_t low = (high <= 9) ? (9 - high) : (25 - high);
  uint8_t lastByte = (high << 4) | low;

  return 0x300000012000ULL | lastByte;
}

// ==================== Sampo OFF Raw ====================
const uint16_t rawDataOff[109] = {
  452,3586,454,544,450,546,476,458,514,546,
  452,544,452,546,452,546,454,544,450,544,
  452,544,452,546,450,548,450,546,448,548,
  450,546,452,546,452,568,426,550,448,548,
  450,546,448,546,450,550,446,548,448,570,
  426,572,422,550,444,574,420,552,442,554,
  444,576,422,572,424,576,420,576,422,576,
  422,574,422,1574,420,556,444,576,420,1574,
  420,576,422,576,420,578,418,576,420,578,
  420,1574,420,576,422,574,420,1574,418,578,
  420,578,420,1572,420,1574,420,3572,420
};

// ==================== IR ====================
void sendOfficeAcCommand(const char* command, int temp,
                         unsigned long mqttNumber, const char* traceId) {
  unsigned long start = millis();

  Serial.println();
  Serial.println("------------------------------------------------------------");
  Serial.println("[TRACE][IR] TRANSMIT START");
  Serial.println("------------------------------------------------------------");
  Serial.print("[TRACE][IR] MQTT#        = "); Serial.println(mqttNumber);
  Serial.print("[TRACE][IR] Command      = "); Serial.println(command);
  Serial.print("[TRACE][IR] Temperature  = "); Serial.println(temp);
  Serial.print("[TRACE][IR] TraceId      = "); Serial.println(traceId);
  Serial.print("[TRACE][IR] GPIO         = "); Serial.println(kIrLed);
  Serial.println("[TRACE][IR] Frequency    = 38 kHz");

  feedWatchdog();

  if (strcmp(command, "turn_on") == 0) {
    int actualTemp = constrain(temp, 16, 30);
    uint64_t code = getSampoCode(actualTemp);

    Serial.println("[TRACE][IR] Mode         = SAMPO CLIMABUTLER");
    Serial.print("[TRACE][IR] Temperature  = ");
    Serial.print(actualTemp);
    Serial.println("°C");
    Serial.print("[TRACE][IR] State        = 0x");
    Serial.println((unsigned long long)code, HEX);

    rawSender.sendClimaButler(code, 52);

  } else if (strcmp(command, "turn_off") == 0) {
    Serial.println("[TRACE][IR] Mode         = RAW OFF CODE");
    Serial.println("[TRACE][IR] Raw length   = 109");
    rawSender.sendRaw(rawDataOff, 109, 38);

  } else {
    Serial.print("[TRACE][IR] UNKNOWN COMMAND = ");
    Serial.println(command);
    Serial.println("[TRACE][IR] RESULT       = NOT_SENT");
    Serial.println("------------------------------------------------------------");
    return;
  }

  feedWatchdog();

  unsigned long elapsed = millis() - start;
  irTransmitCount++;

  Serial.print("[TRACE][IR] TRANSMIT DONE | elapsed = ");
  Serial.print(elapsed);
  Serial.println(" ms");
  Serial.println("[TRACE][IR] RESULT       = SENT");
  Serial.print("[TRACE][IR] IR Count     = ");
  Serial.println(irTransmitCount);
  Serial.println("------------------------------------------------------------");
}

// ==================== MQTT Callback ====================
// 只解析、驗證、放入 pending。
// 不在 callback 裡發 IR。
void callback(char* topic, byte* payload, unsigned int length) {
  mqttMessageCount++;
  unsigned long mqttNumber = mqttMessageCount;

  Serial.println();
  Serial.println("============================================================");
  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.println("] MESSAGE RECEIVED");
  Serial.println("------------------------------------------------------------");

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.print("] Topic       = "); Serial.println(topic);

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.print("] Length      = "); Serial.print(length);
  Serial.println(" bytes");

  if (strcmp(topic, mqtt_topic) != 0) {
    Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
    Serial.println("] RESULT      = IGNORED_WRONG_TOPIC");
    Serial.println("============================================================");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
    Serial.print("] JSON ERROR  = "); Serial.println(error.c_str());
    Serial.println("[TRACE][MQTT] RESULT      = JSON_PARSE_ERROR");
    Serial.println("============================================================");
    return;
  }

  const char* command = doc["command"];
  if (command == nullptr) {
    Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
    Serial.println("] ERROR       = MISSING_COMMAND");
    Serial.println("============================================================");
    return;
  }

  int temp = doc["temperature"] | 25;
  const char* traceId = doc["traceId"] | doc["requestId"] | "N/A";

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.print("] Action      = "); Serial.println(command);

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.print("] Temperature = "); Serial.println(temp);

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.print("] TraceId     = "); Serial.println(traceId);

  if (strcmp(command, "turn_on") != 0 &&
      strcmp(command, "turn_off") != 0) {
    Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
    Serial.print("] ERROR       = UNKNOWN_COMMAND: ");
    Serial.println(command);
    Serial.println("============================================================");
    return;
  }

  bool replaced = pending.valid;

  strncpy(pending.command, command, sizeof(pending.command) - 1);
  pending.command[sizeof(pending.command) - 1] = '\0';

  pending.temperature = temp;

  strncpy(pending.traceId, traceId, sizeof(pending.traceId) - 1);
  pending.traceId[sizeof(pending.traceId) - 1] = '\0';

  pending.receivedAt = millis();
  pending.mqttNumber = mqttNumber;
  pending.valid = true;

  Serial.println();
  Serial.println("[QUEUE] --------------------------------------------------");

  Serial.print("[QUEUE] MQTT message #");
  Serial.print(mqttNumber);
  if (replaced) Serial.println(" REPLACED previous pending command");
  else Serial.println(" queued");

  Serial.print("[QUEUE] Command     = "); Serial.println(pending.command);
  Serial.print("[QUEUE] Temperature  = "); Serial.println(pending.temperature);
  Serial.print("[QUEUE] TraceId      = "); Serial.println(pending.traceId);
  Serial.println("[QUEUE] Policy       = LAST COMMAND WINS");
  Serial.print("[QUEUE] State        = WAIT_SETTLE ");
  Serial.print(COMMAND_SETTLE_MS);
  Serial.println(" ms");
  Serial.println("[QUEUE] --------------------------------------------------");

  Serial.print("[TRACE][MQTT][#"); Serial.print(mqttNumber);
  Serial.println("] RESULT      = ACCEPTED_TO_QUEUE");
  Serial.println("============================================================");
}

// ==================== Queue → IR ====================
void processPendingCommand() {
  if (!pending.valid) return;

  unsigned long now = millis();
  unsigned long waited = now - pending.receivedAt;

  if (waited < COMMAND_SETTLE_MS) return;

  if (lastIrTransmitTime != 0 &&
      now - lastIrTransmitTime < IR_MIN_INTERVAL_MS) {
    return;
  }

  PendingCommand command = pending;
  pending.valid = false;

  Serial.println();
  Serial.println("============================================================");
  Serial.print("[TRACE][QUEUE][MQTT#");
  Serial.print(command.mqttNumber);
  Serial.println("] READY TO EXECUTE");
  Serial.println("------------------------------------------------------------");

  Serial.print("[TRACE][QUEUE] Command     = ");
  Serial.println(command.command);
  Serial.print("[TRACE][QUEUE] Temperature  = ");
  Serial.println(command.temperature);
  Serial.print("[TRACE][QUEUE] TraceId      = ");
  Serial.println(command.traceId);
  Serial.print("[TRACE][QUEUE] Waited       = ");
  Serial.print(waited);
  Serial.println(" ms");
  Serial.println("[TRACE][QUEUE] LAST COMMAND WINS = EXECUTING");

  sendOfficeAcCommand(command.command, command.temperature,
                      command.mqttNumber, command.traceId);

  lastIrTransmitTime = millis();

  Serial.print("[TRACE][QUEUE] RESULT       = EXECUTED | MQTT#");
  Serial.println(command.mqttNumber);
  Serial.println("============================================================");
}

// ==================== Wi-Fi ====================
void setupWiFi() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println("[WiFi] INITIALIZATION");
  Serial.println("------------------------------------------------------------");

  Serial.print("[WiFi] SSID = ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 10000) {
    delay(250);
    Serial.print(".");
    feedWatchdog();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] ★ INITIAL CONNECTION SUCCESS ★");
    Serial.print("[WiFi] IP   = "); Serial.println(WiFi.localIP());
    Serial.print("[WiFi] RSSI = "); Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    wifiDisconnectedSince = 0;
  } else {
    Serial.println("[WiFi] INITIAL CONNECTION FAILED");
    Serial.println("[WiFi] LOOP WILL TRY AUTOMATIC RECOVERY");
  }

  Serial.println("============================================================");
}

void reconnectWiFi() {
  Serial.println();
  Serial.println("[WiFi] Attempting automatic recovery...");
  WiFi.reconnect();
}

void restartWiFiConnection() {
  Serial.println();
  Serial.println("[WiFi] Re-starting WiFi.begin()...");
  WiFi.disconnect(false);
  delay(100);
  feedWatchdog();
  WiFi.begin(ssid, password);
}

// ==================== MQTT ====================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println();
  Serial.println("[MQTT] Attempting HiveMQ connection...");

  String clientId = "ESP32-Office-";
  uint64_t chipId = ESP.getEfuseMac();

  char chipString[17];
  snprintf(chipString, sizeof(chipString), "%llX",
           (unsigned long long)chipId);

  clientId += chipString;
  clientId += "-";
  clientId += String((uint32_t)micros(), HEX);

  Serial.print("[MQTT] Client ID = ");
  Serial.println(clientId);

  if (client.connected()) client.disconnect();

  feedWatchdog();

  bool connected = client.connect(clientId.c_str());

  if (connected) {
    Serial.println("[MQTT] ★ MQTT CONNECTION SUCCESS ★");

    bool subscribed = client.subscribe(mqtt_topic);

    if (subscribed) {
      Serial.print("[MQTT] SUBSCRIBED = ");
      Serial.println(mqtt_topic);
      mqttDisconnectedSince = 0;
    } else {
      Serial.println("[MQTT] SUBSCRIBE FAILED");
    }
  } else {
    Serial.print("[MQTT] CONNECTION FAILED | state = ");
    Serial.println(client.state());
    Serial.println("[MQTT] Will retry automatically");
  }
}

// ==================== Wi-Fi State ====================
void handleWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDisconnectedSince != 0) {
      Serial.println();
      Serial.println("[WiFi] ★ Wi-Fi RESTORED ★");
      Serial.print("[WiFi] IP   = "); Serial.println(WiFi.localIP());
      Serial.print("[WiFi] RSSI = "); Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      wifiDisconnectedSince = 0;
    }
    return;
  }

  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = now;
    Serial.println();
    Serial.println("[WiFi] !!! Wi-Fi DISCONNECTED !!!");
    Serial.println("[WiFi] Automatic recovery started");
  }

  if (now - lastWiFiCheckTime >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheckTime = now;
    reconnectWiFi();
  }

  if (now - lastWiFiBeginTime >= WIFI_BEGIN_INTERVAL) {
    lastWiFiBeginTime = now;
    restartWiFiConnection();
  }

  if (now - wifiDisconnectedSince >= WIFI_RESTART_TIMEOUT) {
    Serial.println();
    Serial.println("[SYSTEM] Wi-Fi unavailable > 5 minutes");
    Serial.println("[SYSTEM] ESP32 RESTART");
    delay(500);
    feedWatchdog();
    ESP.restart();
  }
}

// ==================== MQTT State ====================
void handleMQTT() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  if (client.connected()) {
    mqttDisconnectedSince = 0;
    client.loop();
    return;
  }

  if (mqttDisconnectedSince == 0) {
    mqttDisconnectedSince = now;
    Serial.println();
    Serial.println("[MQTT] !!! MQTT DISCONNECTED !!!");
    Serial.print("[MQTT] State = ");
    Serial.println(client.state());
  }

  if (now - lastMqttConnectAttempt >= MQTT_RECONNECT_INTERVAL) {
    lastMqttConnectAttempt = now;
    reconnectMQTT();
  }

  if (now - mqttDisconnectedSince >= MQTT_RESTART_TIMEOUT) {
    Serial.println();
    Serial.println("[SYSTEM] MQTT unavailable > 10 minutes");
    Serial.println("[SYSTEM] ESP32 RESTART");
    delay(500);
    feedWatchdog();
    ESP.restart();
  }
}

// ==================== Status ====================
void printSystemStatus() {
  unsigned long now = millis();

  if (now - lastStatusReportTime < STATUS_REPORT_INTERVAL) return;
  lastStatusReportTime = now;

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[STATUS] SYSTEM HEALTH");
  Serial.println("------------------------------------------------------------");

  Serial.print("[STATUS] Uptime          = ");
  Serial.print((now - setupTime) / 1000);
  Serial.println(" sec");

  Serial.print("[STATUS] Free Heap       = ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  Serial.print("[STATUS] Min Free Heap   = ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");

  Serial.print("[STATUS] MQTT Messages   = ");
  Serial.println(mqttMessageCount);

  Serial.print("[STATUS] IR Transmits    = ");
  Serial.println(irTransmitCount);

  Serial.print("[STATUS] Pending Command  = ");
  if (pending.valid) {
    Serial.print(pending.command);
    Serial.print(" ");
    Serial.print(pending.temperature);
    Serial.println("C");
  } else {
    Serial.println("NONE");
  }

  Serial.print("[STATUS] WiFi            = ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED | IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" | RSSI=");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("DISCONNECTED");
  }

  Serial.print("[STATUS] MQTT            = ");
  if (client.connected()) {
    Serial.println("CONNECTED");
  } else {
    Serial.print("DISCONNECTED | State=");
    Serial.println(client.state());
  }

  Serial.print("[STATUS] Watchdog        = ");
  Serial.print(WDT_TIMEOUT);
  Serial.println(" sec");

  Serial.println("============================================================");
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println();
  Serial.println("============================================================");
  Serial.println("       ESP32 OFFICE SAMPO AC CONTROLLER");
  Serial.println("       24/7 AUTO RECOVERY + TRACE VERSION");
  Serial.println("============================================================");

  printResetReason();

  Serial.println("[IR] Initializing IR transmitter...");
  ac.begin();
  rawSender.begin();

  Serial.print("[IR] GPIO = ");
  Serial.println(kIrLed);

  Serial.println("[SYSTEM] Initializing Watchdog...");
  initWatchdog();
  feedWatchdog();

  setupWiFi();
  feedWatchdog();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(30);
  client.setBufferSize(512);

  randomSeed((uint32_t)(micros() ^ (uint32_t)ESP.getEfuseMac()));

  setupTime = millis();
  lastWiFiCheckTime = millis();
  lastWiFiBeginTime = millis();
  lastMqttConnectAttempt = 0;
  lastStatusReportTime = millis();

  wifiDisconnectedSince = 0;
  mqttDisconnectedSince = 0;

  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[SYSTEM] ESP32 INITIALIZATION COMPLETE");
  Serial.println("[SYSTEM] Waiting for MQTT command...");
  Serial.println("============================================================");
}

// ==================== Loop ====================
void loop() {
  feedWatchdog();

  unsigned long currentMillis = millis();

  if (REBOOT_INTERVAL_MS > 0 &&
      currentMillis - setupTime >= REBOOT_INTERVAL_MS) {
    Serial.println();
    Serial.println("[SYSTEM] Scheduled restart");
    delay(500);
    feedWatchdog();
    ESP.restart();
  }

  handleWiFi();
  feedWatchdog();

  handleMQTT();
  feedWatchdog();

  // MQTT 收到後在 loop 中才真正發 IR
  processPendingCommand();
  feedWatchdog();

  printSystemStatus();

  delay(10);
}
