#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Kelon.h>

// ============================================================
// ESP32 Kelon AC Controller
// 24/7 Stable + Full Trace Logging Version
//
// API -> MQTT -> ESP32 -> QUEUE -> IR
//
// Features:
// 1. 防快速連點：300ms settle，最後一筆優先
// 2. MQTT callback 不直接發 IR
// 3. 同時支援 command/temperature 與 Action/Temperature
// 4. Wi-Fi 自動恢復
// 5. MQTT 自動恢復
// 6. MQTT socket timeout
// 7. 15 秒 Task Watchdog
// 8. Reset Reason 記錄
// 9. 每 30 秒 Heap / Wi-Fi / MQTT 狀態
// 10. 完整 Trace Log：每筆 MQTT 訊息都有 Sequence ID
// 11. 可選的 requestId / traceId / RequestId / TraceId 追蹤
// 12. IR 發射與狀態碼完整紀錄
// ============================================================

// ============================================================
// Wi-Fi
// ============================================================
const char* ssid = "ice22-1";
const char* password = "19991130";

// ============================================================
// MQTT
// ============================================================
const char* mqtt_server = "broker.hivemq.com";
const uint16_t mqtt_port = 1883;
const char* mqtt_topic = "home/livingroom/ac";

// ============================================================
// Hardware
// ============================================================
const uint16_t kIrLed = 13;

IRKelonAc ac(kIrLed);
IRsend rawSender(kIrLed);

WiFiClient espClient;
PubSubClient client(espClient);

// ============================================================
// Watchdog
// ============================================================
#define WDT_TIMEOUT_SEC 15

// ============================================================
// Timing
// ============================================================
const unsigned long WIFI_CHECK_INTERVAL       = 10000UL;
const unsigned long MQTT_RECONNECT_INTERVAL   = 5000UL;
const unsigned long WIFI_RESTART_TIMEOUT      = 5UL * 60UL * 1000UL;
const unsigned long MQTT_RESTART_TIMEOUT      = 10UL * 60UL * 1000UL;
const unsigned long STATUS_REPORT_INTERVAL    = 30000UL;

// 收到 MQTT 後等待 300ms。
// 如果 300ms 內又收到新溫度，就只執行最後一筆。
const unsigned long COMMAND_SETTLE_MS        = 300UL;

// 兩次 IR 發射至少間隔 800ms。
const unsigned long IR_MIN_INTERVAL_MS       = 800UL;

// 0 = 不使用定期重開。
const unsigned long REBOOT_INTERVAL_MS       = 0UL;

// MQTT payload 最多顯示這麼多 bytes，避免 Serial 爆量。
const size_t MAX_LOG_PAYLOAD = 512;

// ============================================================
// Runtime state
// ============================================================
unsigned long setupTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastMqttConnectAttempt = 0;
unsigned long lastStatusReportTime = 0;
unsigned long wifiDisconnectedSince = 0;
unsigned long mqttDisconnectedSince = 0;
unsigned long lastIrSendTime = 0;

// 每收到一筆 MQTT 就增加一次。
uint32_t mqttMessageSeq = 0;

// 每次 IR 發射增加一次。
uint32_t irSendSeq = 0;

// ============================================================
// Pending command
// ============================================================
bool pendingCommandValid = false;
char pendingCommand[16] = {0};
int pendingTemperature = 27;
unsigned long pendingCommandTime = 0;
uint32_t pendingMessageSeq = 0;
char pendingTraceId[64] = {0};

// ============================================================
// Utility
// ============================================================
const char* yesNo(bool value) {
  return value ? "YES" : "NO";
}

void printHex64(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016llX", (unsigned long long)value);
  Serial.print(buffer);
}

void copySafeString(char* dest, size_t destSize, const char* src) {
  if (destSize == 0) return;
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

// ============================================================
// Reset reason
// ============================================================
const char* getResetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWER_ON";
    case ESP_RST_EXT:       return "EXTERNAL_RESET";
    case ESP_RST_SW:        return "SOFTWARE_RESET";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "OTHER";
  }
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[BOOT] RESET INFORMATION");
  Serial.println("------------------------------------------------------------");
  Serial.print("[BOOT] Reset code   : ");
  Serial.println((int)reason);
  Serial.print("[BOOT] Reset reason : ");
  Serial.println(getResetReasonName(reason));
  Serial.print("[BOOT] Free heap    : ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.print("[BOOT] Min heap     : ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");
  Serial.print("[BOOT] Chip revision: ");
  Serial.println(ESP.getChipRevision());
  Serial.print("[BOOT] CPU          : ");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.println(" MHz");
  Serial.println("============================================================");
}

// ============================================================
// Watchdog
// ============================================================
void initWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif

  esp_task_wdt_add(NULL);
}

// ============================================================
// Kelon IR waveform
// ============================================================
void sendCustomKelon(uint64_t data) {
  uint32_t currentIrSeq = ++irSendSeq;

  Serial.println();
  Serial.println("------------------------------------------------------------");
  Serial.print("[IR][#");
  Serial.print(currentIrSeq);
  Serial.println("] TRANSMIT START");

  Serial.print("[IR][#");
  Serial.print(currentIrSeq);
  Serial.print("] GPIO        = ");
  Serial.println(kIrLed);

  Serial.print("[IR][#");
  Serial.print(currentIrSeq);
  Serial.print("] Frequency   = 38 kHz");
  Serial.println();

  Serial.print("[IR][#");
  Serial.print(currentIrSeq);
  Serial.print("] State       = 0x");
  printHex64(data);
  Serial.println();

  unsigned long start = millis();

  rawSender.enableIROut(38);

  // Header
  rawSender.mark(7318);
  rawSender.space(3758);

  // 48-bit
  for (int i = 0; i < 48; i++) {
    rawSender.mark(530);

    if ((data >> i) & 1ULL) {
      rawSender.space(1294);
    } else {
      rawSender.space(512);
    }

    if ((i & 0x0F) == 0) {
      esp_task_wdt_reset();
    }
  }

  // Footer
  rawSender.mark(530);
  rawSender.space(0);

  unsigned long elapsed = millis() - start;

  Serial.print("[IR][#");
  Serial.print(currentIrSeq);
  Serial.print("] TRANSMIT DONE | elapsed = ");
  Serial.print(elapsed);
  Serial.println(" ms");
  Serial.println("------------------------------------------------------------");
}

// ============================================================
// AC state
// ============================================================
uint64_t getAcState(const char* command, int temp) {
  if (strcmp(command, "turn_off") == 0) {
    return 0x1C000077590EULL;
  }

  if (strcmp(command, "turn_on") == 0) {
    if (temp < 18) temp = 18;
    if (temp > 32) temp = 32;

    uint64_t base = 0x00000077000EULL;
    uint64_t byte5 = (uint64_t)(temp + 4) << 40;
    uint64_t byte1 = (uint64_t)(temp + 155) << 8;

    return base | byte5 | byte1;
  }

  return 0x1C000077590EULL;
}

// ============================================================
// Queue command
// ============================================================
void queueCommand(const char* command,
                  int temp,
                  uint32_t messageSeq,
                  const char* traceId) {

  // 最後一筆優先。
  copySafeString(pendingCommand, sizeof(pendingCommand), command);

  if (strcmp(command, "turn_on") == 0) {
    pendingTemperature = constrain(temp, 18, 32);
  } else {
    pendingTemperature = temp;
  }

  pendingCommandTime = millis();
  pendingMessageSeq = messageSeq;
  copySafeString(pendingTraceId, sizeof(pendingTraceId), traceId);
  pendingCommandValid = true;

  Serial.println();
  Serial.println("[QUEUE] --------------------------------------------------");
  Serial.print("[QUEUE] MQTT message #");
  Serial.print(messageSeq);
  Serial.println(" queued");
  Serial.print("[QUEUE] Command     = ");
  Serial.println(pendingCommand);
  Serial.print("[QUEUE] Temperature = ");
  Serial.println(pendingTemperature);
  Serial.print("[QUEUE] TraceId      = ");
  Serial.println(pendingTraceId[0] ? pendingTraceId : "N/A");
  Serial.println("[QUEUE] Policy       = LAST COMMAND WINS");
  Serial.println("[QUEUE] State        = WAIT_SETTLE");
  Serial.println("[QUEUE] --------------------------------------------------");
}

// ============================================================
// MQTT callback
// 重要：這裡只解析、記錄、排隊，不直接發 IR。
// ============================================================
void callback(char* topic, byte* payload, unsigned int length) {
  esp_task_wdt_reset();

  uint32_t messageSeq = ++mqttMessageSeq;

  Serial.println();
  Serial.println("============================================================");
  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.println("] MESSAGE RECEIVED");
  Serial.println("------------------------------------------------------------");

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.print("] Topic       = ");
  Serial.println(topic);

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.print("] Length      = ");
  Serial.print(length);
  Serial.println(" bytes");

  if (strcmp(topic, mqtt_topic) != 0) {
    Serial.println("[TRACE][MQTT] RESULT = IGNORED_TOPIC");
    Serial.println("============================================================");
    return;
  }

  // ----------------------------------------------------------
  // Raw payload log
  // ----------------------------------------------------------
  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.println("] Payload     = ");

  size_t logLength = min((size_t)length, MAX_LOG_PAYLOAD - 1);
  for (size_t i = 0; i < logLength; i++) {
    char c = (char)payload[i];
    if (c == '\r' || c == '\n') c = ' ';
    Serial.print(c);
  }
  if (length >= MAX_LOG_PAYLOAD) {
    Serial.print("... [TRUNCATED]");
  }
  Serial.println();

  // ----------------------------------------------------------
  // JSON parse
  // ----------------------------------------------------------
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("[TRACE][MQTT][#");
    Serial.print(messageSeq);
    Serial.print("] RESULT      = JSON_PARSE_ERROR: ");
    Serial.println(error.c_str());
    Serial.println("============================================================");
    return;
  }

  // ----------------------------------------------------------
  // 支援兩種格式：
  // 1. command / temperature
  // 2. Action / Temperature
  // ----------------------------------------------------------
  const char* command = "";

  if (doc["command"].is<const char*>()) {
    command = doc["command"];
  }
  else if (doc["Action"].is<const char*>()) {
    command = doc["Action"];
  }

  int temp = 27;

  if (doc["temperature"].is<int>()) {
    temp = doc["temperature"];
  }
  else if (doc["Temperature"].is<int>()) {
    temp = doc["Temperature"];
  }

  // ----------------------------------------------------------
  // Optional trace ID
  // API 如果未提供，就使用本機 MQTT message #。
  // 支援 requestId / RequestId / traceId / TraceId
  // ----------------------------------------------------------
  const char* traceId = "";

  if (doc["requestId"].is<const char*>()) {
    traceId = doc["requestId"];
  }
  else if (doc["RequestId"].is<const char*>()) {
    traceId = doc["RequestId"];
  }
  else if (doc["traceId"].is<const char*>()) {
    traceId = doc["traceId"];
  }
  else if (doc["TraceId"].is<const char*>()) {
    traceId = doc["TraceId"];
  }

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.print("] Action      = ");
  Serial.println(command[0] ? command : "<MISSING>");

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.print("] Temperature = ");
  Serial.println(temp);

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.print("] TraceId     = ");
  Serial.println(traceId[0] ? traceId : "N/A");

  if (command[0] == '\0') {
    Serial.print("[TRACE][MQTT][#");
    Serial.print(messageSeq);
    Serial.println("] RESULT      = REJECTED_MISSING_ACTION");
    Serial.println("============================================================");
    return;
  }

  if (strcmp(command, "turn_on") == 0) {
    queueCommand("turn_on", temp, messageSeq, traceId);
  }
  else if (strcmp(command, "turn_off") == 0) {
    queueCommand("turn_off", 27, messageSeq, traceId);
  }
  else {
    Serial.print("[TRACE][MQTT][#");
    Serial.print(messageSeq);
    Serial.print("] RESULT      = REJECTED_UNKNOWN_ACTION: ");
    Serial.println(command);
    Serial.println("============================================================");
    return;
  }

  Serial.print("[TRACE][MQTT][#");
  Serial.print(messageSeq);
  Serial.println("] RESULT      = ACCEPTED_TO_QUEUE");
  Serial.println("============================================================");
}

// ============================================================
// Process pending command
// ============================================================
void processPendingCommand() {
  if (!pendingCommandValid) {
    return;
  }

  unsigned long now = millis();

  // ----------------------------------------------------------
  // Settle window
  // ----------------------------------------------------------
  if (now - pendingCommandTime < COMMAND_SETTLE_MS) {
    return;
  }

  // ----------------------------------------------------------
  // IR minimum interval
  // ----------------------------------------------------------
  if (lastIrSendTime != 0 &&
      now - lastIrSendTime < IR_MIN_INTERVAL_MS) {
    return;
  }

  char command[sizeof(pendingCommand)];
  copySafeString(command, sizeof(command), pendingCommand);

  int temp = pendingTemperature;
  uint32_t sourceMessageSeq = pendingMessageSeq;

  char traceId[sizeof(pendingTraceId)];
  copySafeString(traceId, sizeof(traceId), pendingTraceId);

  // 先清除 pending。
  // IR 發射期間如果又收到新 MQTT，會建立新的 pending。
  pendingCommandValid = false;

  Serial.println();
  Serial.println("============================================================");
  Serial.print("[TRACE][QUEUE][MQTT#");
  Serial.print(sourceMessageSeq);
  Serial.println("] READY TO EXECUTE");
  Serial.println("------------------------------------------------------------");

  Serial.print("[TRACE][QUEUE] Command     = ");
  Serial.println(command);
  Serial.print("[TRACE][QUEUE] Temperature = ");
  Serial.println(temp);
  Serial.print("[TRACE][QUEUE] TraceId     = ");
  Serial.println(traceId[0] ? traceId : "N/A");
  Serial.print("[TRACE][QUEUE] Waited      = ");
  Serial.print(now - pendingCommandTime);
  Serial.println(" ms");

  // ----------------------------------------------------------
  // Build IR state
  // ----------------------------------------------------------
  uint64_t state = getAcState(command, temp);

  Serial.print("[TRACE][IR] Source MQTT# = ");
  Serial.println(sourceMessageSeq);
  Serial.print("[TRACE][IR] State        = 0x");
  printHex64(state);
  Serial.println();

  if (strcmp(command, "turn_on") == 0) {
    Serial.print("[TRACE][IR] Action       = TURN_ON ");
    Serial.print(temp);
    Serial.println("C");
  }
  else if (strcmp(command, "turn_off") == 0) {
    Serial.println("[TRACE][IR] Action       = TURN_OFF");
  }
  else {
    Serial.println("[TRACE][IR] ERROR        = UNKNOWN_COMMAND");
    Serial.println("============================================================");
    return;
  }

  // ----------------------------------------------------------
  // Send IR
  // ----------------------------------------------------------
  unsigned long irStart = millis();
  sendCustomKelon(state);
  unsigned long irElapsed = millis() - irStart;

  lastIrSendTime = millis();

  Serial.println("------------------------------------------------------------");
  Serial.print("[TRACE][IR] RESULT       = SENT");
  Serial.print(" | duration = ");
  Serial.print(irElapsed);
  Serial.println(" ms");
  Serial.print("[TRACE][IR] MQTT#        = ");
  Serial.println(sourceMessageSeq);
  Serial.print("[TRACE][IR] TraceId       = ");
  Serial.println(traceId[0] ? traceId : "N/A");
  Serial.println("[TRACE][QUEUE] RESULT     = EXECUTED");
  Serial.println("============================================================");

  esp_task_wdt_reset();
}

// ============================================================
// Wi-Fi setup
// ============================================================
void setupWiFi() {
  Serial.println();
  Serial.println("============================================================");
  Serial.println("[TRACE][WIFI] INITIALIZE");
  Serial.println("------------------------------------------------------------");
  Serial.print("[TRACE][WIFI] SSID = ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < 10000UL) {
    delay(250);
    Serial.print(".");
    esp_task_wdt_reset();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[TRACE][WIFI] RESULT = CONNECTED");
    Serial.print("[TRACE][WIFI] IP     = ");
    Serial.println(WiFi.localIP());
    Serial.print("[TRACE][WIFI] RSSI   = ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    wifiDisconnectedSince = 0;
  }
  else {
    Serial.println("[TRACE][WIFI] RESULT = INITIAL_CONNECT_FAILED");
    Serial.println("[TRACE][WIFI] ACTION = AUTO_RECONNECT_IN_LOOP");
  }

  Serial.println("============================================================");
}

// ============================================================
// Wi-Fi reconnect
// ============================================================
void reconnectWiFi() {
  Serial.println();
  Serial.println("[TRACE][WIFI] RECONNECT ATTEMPT");
  Serial.print("[TRACE][WIFI] Status = ");
  Serial.println(WiFi.status());

  WiFi.reconnect();

  esp_task_wdt_reset();
}

// ============================================================
// MQTT reconnect
// ============================================================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TRACE][MQTT] RECONNECT SKIPPED: WIFI_OFFLINE");
    return;
  }

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[TRACE][MQTT] RECONNECT ATTEMPT");
  Serial.println("------------------------------------------------------------");
  Serial.print("[TRACE][MQTT] Broker  = ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.println(mqtt_port);
  Serial.print("[TRACE][MQTT] Topic   = ");
  Serial.println(mqtt_topic);

  char clientId[64];
  uint64_t chipId = ESP.getEfuseMac();
  uint32_t randomPart = esp_random();

  snprintf(clientId,
           sizeof(clientId),
           "ESP32-Kelon-%04X%08X-%08X",
           (uint16_t)(chipId >> 32),
           (uint32_t)chipId,
           randomPart);

  Serial.print("[TRACE][MQTT] Client  = ");
  Serial.println(clientId);

  esp_task_wdt_reset();

  bool connected = client.connect(clientId);

  esp_task_wdt_reset();

  if (connected) {
    Serial.println("[TRACE][MQTT] CONNECT = SUCCESS");

    bool subscribed = client.subscribe(mqtt_topic);

    if (subscribed) {
      mqttDisconnectedSince = 0;
      Serial.println("[TRACE][MQTT] SUBSCRIBE = SUCCESS");
      Serial.print("[TRACE][MQTT] Topic      = ");
      Serial.println(mqtt_topic);
      Serial.println("[TRACE][MQTT] PIPELINE    = READY");
    }
    else {
      Serial.println("[TRACE][MQTT] SUBSCRIBE = FAILED");
      client.disconnect();
    }
  }
  else {
    Serial.print("[TRACE][MQTT] CONNECT = FAILED | state = ");
    Serial.println(client.state());
  }

  Serial.println("============================================================");
}

// ============================================================
// Wi-Fi state management
// ============================================================
void handleWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDisconnectedSince != 0) {
      Serial.println();
      Serial.println("============================================================");
      Serial.println("[TRACE][WIFI] ★ CONNECTION RESTORED ★");
      Serial.print("[TRACE][WIFI] IP   = ");
      Serial.println(WiFi.localIP());
      Serial.print("[TRACE][WIFI] RSSI = ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      Serial.println("============================================================");
      wifiDisconnectedSince = 0;
    }
    return;
  }

  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = now;

    Serial.println();
    Serial.println("============================================================");
    Serial.println("[TRACE][WIFI] !!! DISCONNECTED !!!");
    Serial.println("[TRACE][WIFI] ACTION = AUTO RECOVERY STARTED");
    Serial.println("============================================================");
  }

  if (now - lastWiFiCheckTime >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheckTime = now;
    reconnectWiFi();
  }

  if (now - wifiDisconnectedSince >= WIFI_RESTART_TIMEOUT) {
    Serial.println();
    Serial.println("============================================================");
    Serial.println("[SYSTEM] WIFI RECOVERY TIMEOUT");
    Serial.println("[SYSTEM] Wi-Fi offline for > 5 minutes");
    Serial.println("[SYSTEM] ACTION = ESP.restart()");
    Serial.println("============================================================");
    delay(500);
    ESP.restart();
  }
}

// ============================================================
// MQTT state management
// ============================================================
void handleMQTT() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (client.connected()) {
    mqttDisconnectedSince = 0;

    // 必須高頻率執行。
    client.loop();
    return;
  }

  if (mqttDisconnectedSince == 0) {
    mqttDisconnectedSince = now;

    Serial.println();
    Serial.println("============================================================");
    Serial.println("[TRACE][MQTT] !!! DISCONNECTED !!!");
    Serial.println("[TRACE][MQTT] ACTION = AUTO RECOVERY STARTED");
    Serial.println("============================================================");
  }

  if (now - lastMqttConnectAttempt >= MQTT_RECONNECT_INTERVAL) {
    lastMqttConnectAttempt = now;
    reconnectMQTT();
  }

  if (now - mqttDisconnectedSince >= MQTT_RESTART_TIMEOUT) {
    Serial.println();
    Serial.println("============================================================");
    Serial.println("[SYSTEM] MQTT RECOVERY TIMEOUT");
    Serial.println("[SYSTEM] MQTT offline for > 10 minutes");
    Serial.println("[SYSTEM] ACTION = ESP.restart()");
    Serial.println("============================================================");
    delay(500);
    ESP.restart();
  }
}

// ============================================================
// System status
// ============================================================
void printSystemStatus() {
  unsigned long now = millis();

  if (now - lastStatusReportTime < STATUS_REPORT_INTERVAL) {
    return;
  }

  lastStatusReportTime = now;

  Serial.println();
  Serial.println("============================================================");
  Serial.println("[STATUS] SYSTEM HEALTH");
  Serial.println("------------------------------------------------------------");

  Serial.print("[STATUS] Uptime          = ");
  Serial.print((now - setupTime) / 1000UL);
  Serial.println(" sec");

  Serial.print("[STATUS] Free Heap       = ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  Serial.print("[STATUS] Min Free Heap   = ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");

  Serial.print("[STATUS] MQTT Messages   = ");
  Serial.println(mqttMessageSeq);

  Serial.print("[STATUS] IR Transmits    = ");
  Serial.println(irSendSeq);

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
    Serial.print("DISCONNECTED | state=");
    Serial.println(client.state());
  }

  Serial.print("[STATUS] Pending Command  = ");
  if (pendingCommandValid) {
    Serial.print(pendingCommand);
    Serial.print(" | Temp=");
    Serial.print(pendingTemperature);
    Serial.print(" | MQTT#=");
    Serial.print(pendingMessageSeq);
    Serial.print(" | TraceId=");
    Serial.println(pendingTraceId[0] ? pendingTraceId : "N/A");
  } else {
    Serial.println("NONE");
  }

  Serial.println("============================================================");
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println();
  Serial.println("############################################################");
  Serial.println("# ESP32 KELON AC CONTROLLER");
  Serial.println("# 24/7 STABLE + FULL TRACE LOG");
  Serial.println("# API -> MQTT -> ESP32 -> QUEUE -> IR");
  Serial.println("############################################################");

  // 1. Reset reason 必須最早印。
  printResetReason();

  // 2. Watchdog
  Serial.println("[SYSTEM] Initializing Watchdog...");
  initWatchdog();
  esp_task_wdt_reset();
  Serial.println("[SYSTEM] Watchdog = 15 sec");

  // 3. IR
  Serial.println("[IR] Initializing IR sender...");
  ac.begin();
  rawSender.begin();
  Serial.print("[IR] GPIO = ");
  Serial.println(kIrLed);
  esp_task_wdt_reset();

  // 4. Wi-Fi
  setupWiFi();
  esp_task_wdt_reset();

  // 5. MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(30);
  client.setBufferSize(512);
  client.setSocketTimeout(5);

  // 6. Runtime timers
  setupTime = millis();
  lastWiFiCheckTime = millis();
  lastMqttConnectAttempt = 0;
  lastStatusReportTime = millis();
  wifiDisconnectedSince = 0;
  mqttDisconnectedSince = 0;
  lastIrSendTime = 0;
  pendingCommandValid = false;
  mqttMessageSeq = 0;
  irSendSeq = 0;

  randomSeed((uint32_t)esp_random());

  // 7. MQTT initial connect
  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }

  esp_task_wdt_reset();

  Serial.println();
  Serial.println("############################################################");
  Serial.println("# SYSTEM READY");
  Serial.println("# MQTT -> ESP32 -> IR pipeline is ready");
  Serial.println("############################################################");
  Serial.println();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  // 1. Feed Watchdog
  esp_task_wdt_reset();

  unsigned long currentMillis = millis();

  // 2. Optional periodic reboot
  if (REBOOT_INTERVAL_MS > 0 &&
      currentMillis - setupTime >= REBOOT_INTERVAL_MS) {

    Serial.println();
    Serial.println("[SYSTEM] Periodic reboot triggered");
    delay(500);
    ESP.restart();
  }

  // 3. Wi-Fi
  handleWiFi();

  // 4. MQTT
  handleMQTT();

  // 5. IR queue
  processPendingCommand();

  // 6. Health log
  printSystemStatus();

  // 7. Short yield
  delay(5);
}
