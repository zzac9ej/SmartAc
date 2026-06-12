#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// 紅外線接收器接在 GPIO 14 (D14)
const uint16_t kRecvPin = 14;

// 設定接收緩衝區大小。冷氣遙控器的訊號通常較長，設定為 1024 比較安全
const uint16_t kCaptureBufferSize = 1024;

// 訊號逾時時間 (ms)，冷氣通常需要較長的時間以確保完整讀取
const uint8_t kTimeout = 50;

IRrecv irrecv(kRecvPin, kCaptureBufferSize, kTimeout, true);
decode_results results;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(50); // 等待序列埠連線
  
  Serial.println();
  Serial.print("IRrecvDump 啟動。接收腳位為 GPIO ");
  Serial.println(kRecvPin);
  
  irrecv.enableIRIn(); // 開始接收訊號
  Serial.println("請將冷氣遙控器對準接收器並按下按鍵...");
}

void loop() {
  if (irrecv.decode(&results)) {
    // 輸出協定與詳細資訊
    Serial.println("=========================================");
    Serial.print("協定種類 (Protocol): ");
    Serial.println(typeToString(results.decode_type));
    
    // 如果是已知協定，且屬於冷氣類型，輸出狀態資料
    if (hasACState(results.decode_type)) {
      Serial.print("偵測到冷氣 (A/C) 訊號。狀態長度 (Bits): ");
      Serial.println(results.bits);
      
      // 以十六進位陣列印出狀態值
      Serial.print("狀態碼 (State Hex): ");
      for (uint16_t i = 0; i < results.bits / 8; i++) {
        if (results.state[i] < 16) Serial.print("0");
        Serial.print(results.state[i], HEX);
        if (i < (results.bits / 8) - 1) Serial.print(", ");
      }
      Serial.println();
    } else {
      // 一般電視或其它短碼遙控器
      Serial.print("數值 (Hex): 0x");
      serialPrintUint64(results.value, 16);
      Serial.println();
      Serial.print("資料長度 (Bits): ");
      Serial.println(results.bits);
    }
    
    
    Serial.println("=========================================");
    // 這一行是最關鍵的！會印出底層物理波形的 C++ 陣列
    Serial.println("下方為實體波形陣列 (rawData)，請完整複製：");
    Serial.println(resultToSourceCode(&results));
    Serial.println();
    
    irrecv.resume(); // 繼續等待下一個訊號
  }
  delay(100);
}
