// 辦公室 ESP32 韌體 (Sampo 聲寶冷氣)
#define ROOM_OFFICE

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <Arduino.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Kelon.h>

// ============================================================
// Wi-Fi 設定
// ============================================================
const char* ssid = "ice22-1";
const char* password = "19991130";

// ============================================================
// MQTT 設定
// ============================================================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

#ifdef ROOM_OFFICE
const char* mqtt_topic = "office/ac";
#else
const char* mqtt_topic = "home/livingroom/ac";
#endif

// ============================================================
// 硬體設定
// ============================================================
const uint16_t kIrLed = 13;

IRKelonAc ac(kIrLed);
IRsend rawSender(kIrLed);

WiFiClient espClient;
PubSubClient client(espClient);

// ============================================================
// Watchdog
// ============================================================
#define WDT_TIMEOUT 15

// ============================================================
// 時間設定
// ============================================================

// Wi-Fi 每隔多久檢查一次
const unsigned long WIFI_CHECK_INTERVAL = 5000;

// MQTT 每隔多久嘗試重新連線
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

// Wi-Fi 斷線多久後，強制重啟 ESP32
const unsigned long WIFI_RESTART_TIMEOUT = 5UL * 60UL * 1000UL;

// MQTT 連線失敗多久後，強制重啟 ESP32
const unsigned long MQTT_RESTART_TIMEOUT = 10UL * 60UL * 1000UL;

// 每 30 秒顯示系統狀態
const unsigned long STATUS_REPORT_INTERVAL = 30000;

// 最長運作時間
// 0 = 不啟用 24 小時自動重開
const unsigned long REBOOT_INTERVAL_MS = 0;

// ============================================================
// 狀態變數
// ============================================================

unsigned long setupTime = 0;

unsigned long lastWiFiCheckTime = 0;
unsigned long lastMqttConnectAttempt = 0;
unsigned long lastStatusReportTime = 0;

unsigned long wifiDisconnectedSince = 0;
unsigned long mqttDisconnectedSince = 0;

// ============================================================
// Watchdog 初始化
// ============================================================

void initWatchdog() {

#ifdef ESP_IDF_VERSION_VAL

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_task_wdt_init(&wdt_config);

#else

  esp_task_wdt_init(WDT_TIMEOUT, true);

#endif

#else

  esp_task_wdt_init(WDT_TIMEOUT, true);

#endif

  esp_task_wdt_add(NULL);
}

// ============================================================
// 自訂 Kelon IR 波形
// ============================================================

#ifndef ROOM_OFFICE
void sendCustomKelon(uint64_t data) {

  Serial.println("[IR] 開始發射 Kelon 紅外線");

  rawSender.enableIROut(38);

  // Header
  rawSender.mark(7318);
  rawSender.space(3758);

  // 48-bit
  for (int i = 0; i < 48; i++) {

    rawSender.mark(530);

    if ((data >> i) & 1ULL) {
      rawSender.space(1294);
    }
    else {
      rawSender.space(512);
    }
  }

  // Footer
  rawSender.mark(530);
  rawSender.space(0);

  Serial.println("[IR] 發射完成");
}

// ============================================================
// Kelon 指令產生器
// ============================================================

uint64_t getAcState(String command, int temp) {

  // ----------------------------------------------------------
  // 關機
  // ----------------------------------------------------------

  if (command == "turn_off") {

    return 0x1C000077590EULL;
  }

  // ----------------------------------------------------------
  // 開機
  // ----------------------------------------------------------

  if (command == "turn_on") {

    // 限制溫度
    if (temp < 18)
      temp = 18;

    if (temp > 32)
      temp = 32;

    uint64_t base = 0x00000077000EULL;

    uint64_t byte5 =
      (uint64_t)(temp + 4) << 40;

    uint64_t byte1 =
      (uint64_t)(temp + 155) << 8;

    return base | byte5 | byte1;
  }

  // 未知指令
  return 0x1C000077590EULL;
}
#else
// ============================================================
// 辦公室冷氣控制 (Sampo / CLIMABUTLER 協議)
// ============================================================

// 聲寶冷氣溫度代碼計算 (完整公式，已驗證 16-30°C)
// 格式：0x3000000120XX
// 規律：high nibble = temp - 16
//       low  nibble = 9 - high  (temp 16~25°C, high 0~9)
//                  = 25 - high  (temp 26~30°C, high 10~14)
// 完整對照表：
//   16°C → 0x09   17°C → 0x18   18°C → 0x27   19°C → 0x36
//   20°C → 0x45   21°C → 0x54   22°C → 0x63   23°C → 0x72
//   24°C → 0x81   25°C → 0x90   26°C → 0xAF   27°C → 0xBE
//   28°C → 0xCD   29°C → 0xDC   30°C → 0xEB
uint64_t getSampoCode(int temp) {
  if (temp < 16) temp = 16;
  if (temp > 30) temp = 30;
  uint8_t high = (uint8_t)(temp - 16);
  uint8_t low  = (high <= 9) ? (9 - high) : (25 - high);
  uint8_t lastByte = (high << 4) | low;
  return 0x300000012000ULL | lastByte;
}

void sendOfficeAcCommand(String command, int temp) {
  Serial.println("[IR] 開始發射辦公室冷氣紅外線 (Sampo CLIMABUTLER)");

  if (command == "turn_on") {
    uint64_t code = getSampoCode(temp);
    Serial.print("[IR] 溫度: ");
    Serial.print(temp);
    Serial.print("°C → code = 0x");
    Serial.println((unsigned long long)code, HEX);
    rawSender.sendClimaButler(code, 52);

  } else if (command == "turn_off") {
    // 關機使用錄製到的 rawData (UNKNOWN 協議)
    uint16_t rawDataOff[109] = {
      452, 3586,  454, 544,  450, 546,  476, 458,  514, 546,
      452, 544,  452, 546,  452, 546,  454, 544,  450, 544,
      452, 544,  452, 546,  450, 548,  450, 546,  448, 548,
      450, 546,  452, 546,  452, 568,  426, 550,  448, 548,
      450, 546,  448, 546,  450, 550,  446, 548,  448, 570,
      426, 572,  422, 550,  444, 574,  420, 552,  442, 554,
      444, 576,  422, 572,  424, 576,  420, 576,  422, 576,
      422, 574,  422, 1574,  420, 556,  444, 576,  420, 1574,
      420, 576,  422, 576,  420, 578,  418, 576,  420, 578,
      420, 1574,  420, 576,  422, 574,  420, 1574,  418, 578,
      420, 578,  420, 1572,  420, 1574,  420, 3572,  420
    };
    rawSender.sendRaw(rawDataOff, 109, 38);
  }

  Serial.println("[IR] 發射完成");
}
#endif

// ============================================================
// MQTT Callback
// ============================================================

void callback(
  char* topic,
  byte* payload,
  unsigned int length
) {

  Serial.println();
  Serial.println("====================================");
  Serial.println("[MQTT] 收到訊息");
  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);

  // ----------------------------------------------------------
  // Topic 檢查
  // ----------------------------------------------------------

  if (String(topic) != mqtt_topic) {

    Serial.println("[MQTT] Topic 不符合，忽略");
    Serial.println("====================================");

    return;
  }

  // ----------------------------------------------------------
  // JSON 解析
  // ----------------------------------------------------------

  JsonDocument doc;

  DeserializationError error =
    deserializeJson(doc, payload, length);

  if (error) {

    Serial.print("[MQTT] JSON 解析失敗: ");
    Serial.println(error.c_str());

    Serial.println("====================================");

    return;
  }

  // ----------------------------------------------------------
  // 取得 command
  // ----------------------------------------------------------

  const char* command =
    doc["command"];

  if (command == nullptr) {

    Serial.println("[MQTT] 缺少 command");

    Serial.println("====================================");

    return;
  }

  // ----------------------------------------------------------
  // 取得溫度
  // ----------------------------------------------------------

  int temp =
    doc["temperature"] | 25; // 辦公室預設 25°C

  Serial.print("[MQTT] command = ");
  Serial.println(command);

  Serial.print("[MQTT] temperature = ");
  Serial.println(temp);

  // ----------------------------------------------------------
  // 開機
  // ----------------------------------------------------------

  if (String(command) == "turn_on") {

    Serial.print("[AC] 開機，設定溫度: ");
    Serial.print(temp);
    Serial.println("°C");

#ifndef ROOM_OFFICE
    uint64_t state =
      getAcState("turn_on", temp);

    Serial.print("[IR] State = 0x");
    Serial.println(
      (unsigned long long)state,
      HEX
    );

    sendCustomKelon(state);
#else
    sendOfficeAcCommand("turn_on", temp);
#endif
  }

  // ----------------------------------------------------------
  // 關機
  // ----------------------------------------------------------

  else if (String(command) == "turn_off") {

    Serial.println("[AC] 關機");

#ifndef ROOM_OFFICE
    uint64_t state =
      getAcState("turn_off", 0);

    Serial.print("[IR] State = 0x");
    Serial.println(
      (unsigned long long)state,
      HEX
    );

    sendCustomKelon(state);
#else
    sendOfficeAcCommand("turn_off", temp);
#endif
  }

  // ----------------------------------------------------------
  // 未知 command
  // ----------------------------------------------------------

  else {

    Serial.print("[MQTT] 未知 command: ");
    Serial.println(command);
  }

  Serial.println("====================================");
}

// ============================================================
// Wi-Fi 初始化
// ============================================================

void setupWiFi() {

  Serial.println();
  Serial.println("====================================");
  Serial.println("[WiFi] 初始化 Wi-Fi");
  Serial.print("[WiFi] SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);

  // 自動重新連線
  WiFi.setAutoReconnect(true);

  // 避免睡眠造成 MQTT / IR 控制延遲
  WiFi.setSleep(false);

  WiFi.begin(ssid, password);

  unsigned long startTime = millis();

  // 最多等待 10 秒
  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < 10000
  ) {

    delay(250);

    Serial.print(".");

    esp_task_wdt_reset();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("[WiFi] 初始連線成功");

    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("[WiFi] RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    wifiDisconnectedSince = 0;
  }

  else {

    Serial.println("[WiFi] 初始連線失敗");
    Serial.println("[WiFi] 將在 loop() 自動重連");
  }

  Serial.println("====================================");
}

// ============================================================
// Wi-Fi 重新連線
// ============================================================

void reconnectWiFi() {

  Serial.println();
  Serial.println("[WiFi] 嘗試重新連線...");

  // 清除目前連線狀態
  WiFi.disconnect(false);

  delay(100);

  WiFi.begin(ssid, password);
}

// ============================================================
// MQTT 重新連線
// ============================================================

void reconnectMQTT() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.println();
  Serial.println("[MQTT] 嘗試重新連線 HiveMQ...");

  // 建立新的 Client ID
  String clientId = "ESP32-Kelon-";

  clientId +=
    String((uint32_t)ESP.getEfuseMac(), HEX);

  clientId += "-";

  clientId +=
    String(random(0xffff), HEX);

  Serial.print("[MQTT] Client ID: ");
  Serial.println(clientId);

  // ----------------------------------------------------------
  // 先確保舊 MQTT 狀態清除
  // ----------------------------------------------------------

  if (client.connected()) {

    client.disconnect();
  }

  // ----------------------------------------------------------
  // 建立 MQTT
  // ----------------------------------------------------------

  bool connected =
    client.connect(clientId.c_str());

  if (connected) {

    Serial.println("[MQTT] ★ MQTT 連線成功 ★");

    // --------------------------------------------------------
    // 重新訂閱
    // --------------------------------------------------------

    bool subscribed =
      client.subscribe(mqtt_topic);

    if (subscribed) {

      Serial.print("[MQTT] 已訂閱: ");
      Serial.println(mqtt_topic);

      mqttDisconnectedSince = 0;
    }

    else {

      Serial.println("[MQTT] 訂閱失敗");
    }
  }

  else {

    Serial.print("[MQTT] 連線失敗，狀態碼: ");
    Serial.println(client.state());

    Serial.println(
      "[MQTT] 稍後會再次嘗試"
    );
  }
}

// ============================================================
// Wi-Fi 狀態管理
// ============================================================

void handleWiFi() {

  unsigned long now = millis();

  // ----------------------------------------------------------
  // Wi-Fi 正常
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {

    // 如果之前曾經斷線
    if (wifiDisconnectedSince != 0) {

      Serial.println();
      Serial.println(
        "[WiFi] ★ Wi-Fi 已恢復 ★"
      );

      Serial.print("[WiFi] IP: ");
      Serial.println(WiFi.localIP());

      wifiDisconnectedSince = 0;
    }

    return;
  }

  // ----------------------------------------------------------
  // 第一次偵測斷線
  // ----------------------------------------------------------

  if (wifiDisconnectedSince == 0) {

    wifiDisconnectedSince = now;

    Serial.println();
    Serial.println(
      "[WiFi] !!! Wi-Fi 斷線 !!!"
    );

    Serial.println(
      "[WiFi] 開始進行自動恢復"
    );
  }

  // ----------------------------------------------------------
  // 定期重連
  // ----------------------------------------------------------

  if (
    now - lastWiFiCheckTime >=
    WIFI_CHECK_INTERVAL
  ) {

    lastWiFiCheckTime = now;

    reconnectWiFi();
  }

  // ----------------------------------------------------------
  // 長時間無法恢復
  // ----------------------------------------------------------

  if (
    now - wifiDisconnectedSince >=
    WIFI_RESTART_TIMEOUT
  ) {

    Serial.println();
    Serial.println(
      "[SYSTEM] Wi-Fi 已超過 5 分鐘無法恢復"
    );

    Serial.println(
      "[SYSTEM] 自動重新啟動 ESP32"
    );

    delay(1000);

    ESP.restart();
  }
}

// ============================================================
// MQTT 狀態管理
// ============================================================

void handleMQTT() {

  unsigned long now = millis();

  // ----------------------------------------------------------
  // Wi-Fi 還沒恢復
  // ----------------------------------------------------------

  if (WiFi.status() != WL_CONNECTED) {

    return;
  }

  // ----------------------------------------------------------
  // MQTT 正常
  // ----------------------------------------------------------

  if (client.connected()) {

    mqttDisconnectedSince = 0;

    client.loop();

    return;
  }

  // ----------------------------------------------------------
  // 第一次偵測 MQTT 斷線
  // ----------------------------------------------------------

  if (mqttDisconnectedSince == 0) {

    mqttDisconnectedSince = now;

    Serial.println();
    Serial.println(
      "[MQTT] !!! MQTT 斷線 !!!"
    );
  }

  // ----------------------------------------------------------
  // 定期重新連線
  // ----------------------------------------------------------

  if (
    now - lastMqttConnectAttempt >=
    MQTT_RECONNECT_INTERVAL
  ) {

    lastMqttConnectAttempt = now;

    reconnectMQTT();
  }

  // ----------------------------------------------------------
  // MQTT 長時間無法恢復
  // ----------------------------------------------------------

  if (
    now - mqttDisconnectedSince >=
    MQTT_RESTART_TIMEOUT
  ) {

    Serial.println();
    Serial.println(
      "[SYSTEM] MQTT 已超過 10 分鐘無法恢復"
    );

    Serial.println(
      "[SYSTEM] 自動重新啟動 ESP32"
    );

    delay(1000);

    ESP.restart();
  }
}

// ============================================================
// 系統狀態監控
// ============================================================

void printSystemStatus() {

  unsigned long now = millis();

  if (
    now - lastStatusReportTime <
    STATUS_REPORT_INTERVAL
  ) {

    return;
  }

  lastStatusReportTime = now;

  Serial.println();
  Serial.println("--------------- SYSTEM STATUS ---------------");

  // Uptime
  Serial.print("Uptime: ");
  Serial.print(
    (now - setupTime) / 1000
  );
  Serial.println(" sec");

  // Heap
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  // Wi-Fi
  Serial.print("WiFi: ");

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("CONNECTED");

    Serial.print(" | IP=");
    Serial.print(WiFi.localIP());

    Serial.print(" | RSSI=");
    Serial.print(WiFi.RSSI());

    Serial.println(" dBm");
  }

  else {

    Serial.println("DISCONNECTED");
  }

  // MQTT
  Serial.print("MQTT: ");

  if (client.connected()) {

    Serial.println("CONNECTED");
  }

  else {

    Serial.print("DISCONNECTED");

    Serial.print(" | State=");
    Serial.println(client.state());
  }

  Serial.println("----------------------------------------------");
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println();
  Serial.println("====================================");
  Serial.println("   ESP32 Kelon AC Controller");
  Serial.println("   Auto Recovery Version");
  Serial.println("====================================");

  // ----------------------------------------------------------
  // 初始化 IR
  // ----------------------------------------------------------

  Serial.println("[IR] 初始化紅外線");

  ac.begin();

  rawSender.begin();

  Serial.print("[IR] GPIO: ");
  Serial.println(kIrLed);

  // ----------------------------------------------------------
  // 初始化 Watchdog
  // ----------------------------------------------------------

  Serial.println("[SYSTEM] 初始化 Watchdog");

  initWatchdog();

  // ----------------------------------------------------------
  // 初始化 Wi-Fi
  // ----------------------------------------------------------

  setupWiFi();

  // ----------------------------------------------------------
  // 初始化 MQTT
  // ----------------------------------------------------------

  client.setServer(
    mqtt_server,
    mqtt_port
  );

  client.setCallback(callback);

  // MQTT Keep Alive
  client.setKeepAlive(30);

  // MQTT Buffer
  client.setBufferSize(512);

  // ----------------------------------------------------------
  // 初始化時間
  // ----------------------------------------------------------

  setupTime = millis();

  lastWiFiCheckTime = millis();

  lastMqttConnectAttempt = 0;

  lastStatusReportTime = millis();

  wifiDisconnectedSince = 0;

  mqttDisconnectedSince = 0;

  // ----------------------------------------------------------
  // 如果 Wi-Fi 已連線
  // 立即嘗試 MQTT
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {

    reconnectMQTT();
  }

  Serial.println();
  Serial.println("====================================");
  Serial.println("ESP32 初始化完成");
  Serial.println("等待 MQTT 指令...");
  Serial.println("====================================");
}

// ============================================================
// Loop
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // 1. Watchdog
  // ----------------------------------------------------------

  esp_task_wdt_reset();

  unsigned long currentMillis =
    millis();

  // ----------------------------------------------------------
  // 2. 可選的定期重啟
  // ----------------------------------------------------------

  if (
    REBOOT_INTERVAL_MS > 0 &&
    currentMillis - setupTime >=
      REBOOT_INTERVAL_MS
  ) {

    Serial.println();
    Serial.println(
      "[SYSTEM] 達到定期重啟時間"
    );

    Serial.println(
      "[SYSTEM] ESP32 Restart"
    );

    delay(1000);

    ESP.restart();
  }

  // ----------------------------------------------------------
  // 3. Wi-Fi 管理
  // ----------------------------------------------------------

  handleWiFi();

  // ----------------------------------------------------------
  // 4. MQTT 管理
  // ----------------------------------------------------------

  handleMQTT();

  // ----------------------------------------------------------
  // 5. 系統狀態監控
  // ----------------------------------------------------------

  printSystemStatus();

  // ----------------------------------------------------------
  // 6. 讓 CPU 稍微休息
  // ----------------------------------------------------------

  delay(10);
}