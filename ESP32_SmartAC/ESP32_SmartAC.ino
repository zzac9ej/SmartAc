#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// 載入紅外線控制庫
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Kelon.h>

// ==========================================
// ⚠️ 請在這裡填入你家的 WiFi 資訊 ⚠️
// ==========================================
const char* ssid = "ice22-1";          // 替換成你家 WiFi 名稱
const char* password = "19991130";  // 替換成你家 WiFi 密碼

// 與 C# API appsettings.json 對應的 MQTT 設定
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;                   
const char* mqtt_topic = "home/livingroom/ac"; // 接收指令的頻道
// ==========================================
// 硬體腳位設定
// ==========================================
const uint16_t kIrLed = 13; // 改用 P13 腳位，避開無效的 P4 與共用的 P2
IRKelonAc ac(kIrLed);
IRsend rawSender(kIrLed); // 新增一個用來發射物理波形的物件

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// 系統穩定度與非阻塞連線管理設定
// ==========================================
#define WDT_TIMEOUT 10                        // 看門狗超時時間 (秒)
const unsigned long REBOOT_INTERVAL_MS = 86400000; // 定期自動重啟時間 (24 小時)

unsigned long setupTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastMqttConnectAttempt = 0;
unsigned long lastHeapReportTime = 0;

// 看門狗初始化 (相容 ESP32 Arduino Core 2.x 與 3.x)
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
  esp_task_wdt_add(NULL); // 監視主 loop 任務
}

// ==========================================
// 客製化 Kelon 波形發射器
// 解決標準函式庫時序 (9000ms) 與您的冷氣 (7318ms) 不相容的問題
// ==========================================
void sendCustomKelon(uint64_t data) {
  rawSender.enableIROut(38); // 38kHz 頻率
  rawSender.mark(7318);      // 專屬您的 Header Mark
  rawSender.space(3758);     // 專屬您的 Header Space
  
  // 依序發射 48 bits (LSB first)
  for (int i = 0; i < 48; i++) {
    rawSender.mark(530);     // Bit Mark
    if ((data >> i) & 1) {
      rawSender.space(1294); // Bit 1 Space
    } else {
      rawSender.space(512);  // Bit 0 Space
    }
  }
  // Footer
  rawSender.mark(530);
  rawSender.space(0);
}

// ==========================================
// 終極密碼產生器 (字典查表與規律推算)
// ==========================================
uint64_t getAcState(String command, int temp) {
  if (command == "turn_off") {
    // 這是您親手錄下來的關機絕對密碼
    return 0x1C000077590EULL; 
  }
  
  if (command == "turn_on") {
    // 依據您錄製的 28~30 度規律，自動推算任意溫度的密碼
    // 28度: 0x20000077B70E
    // 29度: 0x21000077B80E
    // 30度: 0x22000077B90E
    
    // 限制溫度安全範圍 18~32
    if (temp < 18) temp = 18;
    if (temp > 32) temp = 32;
    
    uint64_t base = 0x00000077000EULL;
    uint64_t byte5 = (uint64_t)(temp + 4) << 40;      // 規律: 溫度 + 4
    uint64_t byte1 = (uint64_t)(temp + 155) << 8;     // 規律: 溫度 + 155
    
    return base | byte5 | byte1;
  }
  
  return 0x1C000077590EULL; // 預設防呆回傳關機
}

// 收到 MQTT 訊息時的處理邏輯
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("收到雲端指令 (Topic: ");
  Serial.print(topic);
  Serial.print("): ");
  Serial.println(message);

  // 確認主題正確
  if (String(topic) == mqtt_topic) {
    // 使用 ArduinoJson 7 的新寫法
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);

    if (error) {
      Serial.print("JSON 解析失敗: ");
      Serial.println(error.c_str());
      return;
    }

    const char* command = doc["command"];
    // 如果 JSON 中沒有傳溫度，預設使用 26 度
    int temp = doc["temperature"] | 27;

    if (String(command) == "turn_on") {
      Serial.print("正在發射: 動態開機指令 (溫度: ");
      Serial.print(temp);
      Serial.println("度, 完美客製化波形)");
      
      // 1. 直接用我們推算出來的終極密碼規律，算出這個溫度的專屬密碼
      uint64_t state = getAcState("turn_on", temp);
      
      // 2. 用我們自己寫的完美節拍器發射 1 次 (避免冷氣連叫三次)
      sendCustomKelon(state);
      
      Serial.println("發射完畢！");
    } 
    else if (String(command) == "turn_off") {
      Serial.println("正在發射: 關機指令 (完美客製化波形)");
      
      // 拿取字典裡的專屬關機密碼
      uint64_t state = getAcState("turn_off", 0);
      
      sendCustomKelon(state);
      
      Serial.println("發射完畢！");
    }
  }
}

void setup_wifi_nonblocking() {
  Serial.println();
  Serial.print("正在初始化 WiFi 連線至: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  
  // 啟動時最多等待 5 秒 (避免完全無 WiFi 時開機卡死)
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset(); // 確保此期間不觸發看門狗
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 初始連線成功！");
    Serial.print("IP 位址: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi 初始連線超時，將在 loop() 中背景繼續嘗試...");
  }
}

void setup() {
  // 0. 關閉低電壓偵測 (Brownout Detector)
  // 避免 WiFi 重連的電流突波在 USB 供電不穩時觸發重啟，發出 USB 拔插音效
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(115200);
  
  // 初始化紅外線發射器
  ac.begin();
  rawSender.begin();
  
  // 初始化軟體看門狗
  initWatchdog();
  
  // 非阻塞 WiFi 初始化
  setup_wifi_nonblocking();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  
  // 初始化時間標籤
  setupTime = millis();
  lastWiFiCheckTime = millis();
  lastMqttConnectAttempt = 0;
  lastHeapReportTime = millis();
}

void loop() {
  // 1. 餵狗：重設看門狗計時器
  esp_task_wdt_reset();
  
  unsigned long currentMillis = millis();
  
  // 2. 定期自動重啟 (24 小時)，清除 Heap 記憶體碎片
  if (currentMillis - setupTime >= REBOOT_INTERVAL_MS) {
    Serial.println("已達到 24 小時定期自動重啟時間，系統重啟中...");
    delay(1000);
    ESP.restart();
  }
  
  // 3. 定期列印系統監控資訊 (每 30 秒)
  if (currentMillis - lastHeapReportTime >= 30000) {
    lastHeapReportTime = currentMillis;
    Serial.print("[系統監控] 剩餘 Heap 記憶體: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" Bytes | WiFi 狀態: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "已連線" : "斷線");
    Serial.print(" | MQTT 狀態: ");
    Serial.println(client.connected() ? "已連線" : "斷線");
  }
  
  // 4. WiFi 連線管理
  if (WiFi.status() != WL_CONNECTED) {
    // 每 10 秒手動觸發一次 WiFi.begin()，在背景嘗試重連，不重啟晶片
    if (currentMillis - lastWiFiCheckTime >= 10000) {
      lastWiFiCheckTime = currentMillis;
      Serial.println("偵測到 WiFi 斷線，嘗試重新呼叫 WiFi.begin()...");
      WiFi.begin(ssid, password);
    }
    
    // WiFi 沒連上，直接結束 loop，下次重試 (不處理 MQTT，也不會發出 USB 重新連線音效)
    return;
  }
  
  // 5. MQTT 連線管理
  if (!client.connected()) {
    // 每 10 秒嘗試非阻塞式連接 MQTT，在背景重連，不重啟晶片
    if (currentMillis - lastMqttConnectAttempt >= 10000) {
      lastMqttConnectAttempt = currentMillis;
      Serial.print("嘗試連接 MQTT 伺服器 (HiveMQ)...");
      
      String clientId = "ESP32Client-";
      clientId += String(random(0xffff), HEX);
      
      if (client.connect(clientId.c_str())) {
        Serial.println("MQTT 連線成功！");
        client.subscribe(mqtt_topic);
      } else {
        Serial.print("MQTT 連線失敗，狀態碼: ");
        Serial.println(client.state());
      }
    }
  } else {
    // 6. 正常處理 MQTT 訂閱與心跳
    client.loop();
  }
}
