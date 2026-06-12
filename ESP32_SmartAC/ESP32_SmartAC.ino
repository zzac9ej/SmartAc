#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

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
      
      // 2. 用我們自己寫的完美節拍器發射 3 次
      sendCustomKelon(state);
      delay(40);
      sendCustomKelon(state);
      delay(40);
      sendCustomKelon(state);
      
      Serial.println("發射完畢！");
    } 
    else if (String(command) == "turn_off") {
      Serial.println("正在發射: 關機指令 (完美客製化波形)");
      
      // 拿取字典裡的專屬關機密碼
      uint64_t state = getAcState("turn_off", 0);
      
      sendCustomKelon(state);
      delay(40);
      sendCustomKelon(state);
      
      Serial.println("發射完畢！");
    }
  }
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("正在連線至 WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi 連線成功！");
  Serial.print("IP 位址: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  // 一直嘗試連線直到成功
  while (!client.connected()) {
    Serial.print("嘗試連接 MQTT 伺服器 (HiveMQ)...");
    
    // 產生一個隨機的 Client ID
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("連線成功！");
      // 訂閱冷氣控制頻道
      client.subscribe(mqtt_topic);
      Serial.print("已訂閱頻道: ");
      Serial.println(mqtt_topic);
    } else {
      Serial.print("失敗，錯誤代碼=");
      Serial.print(client.state());
      Serial.println("。將在 5 秒後重試...");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // 初始化紅外線發射器
  ac.begin();
  rawSender.begin();
  
  setup_wifi();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}
