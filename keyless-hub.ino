#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include <PubSubClient.h> // 請確認已安裝 PubSubClient Library
#include "config.h"

// ====== 硬體腳位設定 (保留您的原始設定) ======
#define checkBluePin 34   // 手機藍牙偵測
#define checkAccPin 35    // 汽車 ACC 偵測
#define checkHumanPin 33  // 人體/敲擊偵測
#define RELAY_PIN_LOCK 5  // 鎖門
#define RELAY_PIN_BOOT 4  // 發車
#define RELAY_PIN_OPEN 15 // 開門
#define POWER_PIN 26      // 鑰匙電源
#define R1_PIN 27         // 備用

// ====== OwnTracks/Command 設定 ======
const char *topic_cmd = "owntracks/" USER_ID "/" DEVICE_ID;

// ====== 全域變數 ======
int preAct = 0;
int carBootSts = 0;
int lastAccState = -1; // 用來偵測 ACC 狀態變化的變數
int key_link = 0;      // MQTT 控制連動
bool is_acting = false; // 是否正在執行動作

// MQTT 物件
WiFiClient espClient;
PubSubClient client(espClient);

// 函式宣告
void setup_wifi();
void reconnect();
void callback(char *topic, byte *payload, unsigned int length);
void triggerBoot();
void unlockDoor();
void lockDoor();
void closeWindow(); // 關窗 
void OpenWindow();  // 開窗
void SendCarPowerMsg(int sts);
void checkPinStates();
bool checkOpenDoor();

void setup()
{
  Serial.begin(115200);

  // --- 腳位設定 ---
  pinMode(checkBluePin, INPUT);
  pinMode(checkAccPin, INPUT);
  pinMode(checkHumanPin, INPUT_PULLDOWN);
  pinMode(RELAY_PIN_OPEN, OUTPUT);
  pinMode(RELAY_PIN_LOCK, OUTPUT);
  pinMode(RELAY_PIN_BOOT, OUTPUT);
  pinMode(POWER_PIN, OUTPUT);
  pinMode(R1_PIN, OUTPUT);

  // 初始狀態 (Relay High 為關閉)
  digitalWrite(RELAY_PIN_OPEN, HIGH);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  digitalWrite(RELAY_PIN_BOOT, HIGH);
  digitalWrite(POWER_PIN, LOW);
  digitalWrite(R1_PIN, LOW);

  // --- 初始化 ACC 狀態 ---
  lastAccState = digitalRead(checkAccPin);
  carBootSts = (lastAccState == HIGH) ? 1 : 0;

  // --- 連接 WiFi ---
  setup_wifi();

  // --- 設定 MQTT ---
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(callback); // 設定收到訊息的處理函式

  // --- [修改 4] MQTT 防斷線機制: KeepAlive ---
  // 設定 60 秒發送一次心跳，避免 4G NAT 斷線
  client.setKeepAlive(60);

  Serial.println("系統啟動完成，進入 Modem Sleep 監聽模式");
}

void loop()
{
  // --- MQTT 連線維護 ---
  if (!client.connected())
  {
    reconnect();
  }
  client.loop(); // 這裡非常重要！它負責處理 MQTT 訊息接收與心跳包

  // ---  處理腳位邏輯 ---
  checkPinStates();
  Serial.print("POWER_PIN state: ");
  Serial.println(digitalRead(POWER_PIN));
  // 這裡的 delay 雖然短，但在 Modem Sleep 模式下，
  // 只要 CPU 空閒，WiFi 模組就會自動尋找機會關閉射頻省電。
  delay(10);
}

// ------------------ 邏輯功能區 ------------------

// [修改 2] 處理 MQTT 收到的訊息 (取代原本 bootCar 的 HTTP GET)
void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("收到指令 [");
  Serial.print(topic);
  Serial.print("]: ");

  String msg = "";
  for (int i = 0; i < length; i++)
  {
    msg += (char)payload[i];
  }
  Serial.println(msg);

  // 解析mqtt指令
  if (msg.indexOf("boot") != -1)
  {
    triggerBoot();
    carBootSts = 1;
    // SendCarPowerMsg(1);
  }
  else if (msg.indexOf("lock") != -1 && msg.indexOf("unlock") == -1)
  {
    lockDoor();
  }
  else if (msg.indexOf("unlock") != -1)
  {
    unlockDoor();
  }
  else if (msg.indexOf("key_on") != -1)
  {
    key_link = 1;
    Serial.println("key_link set to 1");
  }
  else if (msg.indexOf("key_off") != -1)
  {
    key_link = 0;
    Serial.println("key_link set to 0");
  }
  else if (msg.indexOf("window_open") != -1)
  {
    OpenWindow();
  }
  else if (msg.indexOf("window_close") != -1)
  {
    closeWindow();
  }
}

// 綜合狀態檢查函式 (在 loop 中執行)
void checkPinStates()
{
  int currentAcc = digitalRead(checkAccPin);
  int currentBlue = digitalRead(checkBluePin);
  int currentHuman = digitalRead(checkHumanPin);
  //藍牙連上時，電池通電
  if (currentBlue == HIGH)
  {
    digitalWrite(R1_PIN, HIGH);
  }
  else
  {
    digitalWrite(R1_PIN, LOW);
  }

  // --- [新增] Power Pin 控制邏輯 ---
  if (!is_acting)
  {
    if (currentBlue == HIGH && key_link == 1)
    {
      digitalWrite(POWER_PIN, HIGH);
    }
    else
    {
      digitalWrite(POWER_PIN, LOW);
    }
  }

  // --- 1. 偵測 ACC 狀態變化 ---
  if (currentAcc != lastAccState)
  {
    delay(50); // 去抖動
    if (digitalRead(checkAccPin) == currentAcc)
    {
      if (currentAcc == LOW)
      {
        // ACC 從 ON 變 OFF
        Serial.println("汽車關閉 (ACC OFF)");
        SendCarPowerMsg(0);
        carBootSts = 0;
      }
      else
      {
        // ACC 從 OFF 變 ON
        Serial.println("汽車發動 (ACC ON)");
        carBootSts = 1;
        // SendCarPowerMsg(1);
      }
      lastAccState = currentAcc;
    }
  }

  // --- 2. 有藍牙且有人靠近 (開門條件) ---
  if (currentBlue == HIGH && currentAcc == LOW)
  {
    if (checkOpenDoor())
    {
      unlockDoor();
      // 避免連續觸發
      delay(2000);
    }
  }
  // --- 3. 自動鎖門邏輯 (無藍牙且原本開門狀態) ---
  if (currentBlue == LOW && preAct == 1 && currentAcc == LOW)
  {
    // 可再嘗試使用 millis() 進行非阻塞式等待優化
    Serial.println("藍牙中斷，5秒後嘗試鎖門...");
    delay(5000);
    // 再次確認藍牙狀態
    if (digitalRead(checkBluePin) == LOW)
    {
      lockDoor();
    }
  }
}

// 開門檢測邏輯
bool checkOpenDoor()
{
  // 注意：此函式在 loop 中被呼叫，若使用 while 迴圈會暫時卡住 MQTT 接收
  // 但因為這是在確認開門意圖，短暫延遲是可以接受的

  const unsigned long checkDuration = 3000; // 3秒檢測
  unsigned long startCheck = millis();

  while (millis() - startCheck < checkDuration)
  {
    // 隨時保持 MQTT 接收，避免因為卡在這裡 3 秒導致命令延遲
    // client.loop();避免網路在這裡死掉重連，造成等待，下次loop程式會重新處理連線，所以不用在這等待

    if (digitalRead(checkAccPin) == HIGH)
    {
      return false;
    }
    if (digitalRead(checkBluePin) == LOW)
    {
      return false;
    }
    if (digitalRead(checkHumanPin) == HIGH)
    {
      Serial.println("GPIO33 HIGH → 觸發開門");
      return true;
    }
    delay(100);
  }
  return false;
}

void setup_wifi()
{
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA); // 設定為 Station 模式
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int iCount = 0;
  while (WiFi.status() != WL_CONNECTED && iCount <= 20)
  {
    delay(500);
    Serial.print(".");
    iCount++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi 連接成功");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // ====== [修改 1] 這裡就是 Modem Sleep 的開關！ ======
    // 當 Wi-Fi 連線成功後，明確告訴 ESP32 開啟省電模式。
    // 這行執行後，底層會在沒有資料傳輸時，自動關閉 Wi-Fi 射頻。
    WiFi.setSleep(true);
    Serial.println("Modem Sleep 已啟用");
    // =================================================
  }
  else
  {
    Serial.println("\nWiFi 連接失敗，將於 loop 中重試");
  }
}

// [修改 4] MQTT 斷線重連機制
void reconnect()
{
  // 如果 WiFi 斷了，先重連 WiFi
  if (WiFi.status() != WL_CONNECTED)
  {
    setup_wifi();
  }

  // 檢查 MQTT 是否連線
  if (!client.connected())
  {
    Serial.print("Attempting MQTT connection...");

    // 產生隨機 Client ID
    String clientId = "ESP32-Car-" + String(random(0xffff), HEX);

    // 使用設定的帳號密碼連線
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS))
    {
      Serial.println("connected");

      // 連線成功後，重新訂閱命令頻道
      client.subscribe(topic_cmd);
      Serial.println("已訂閱頻道: " + String(topic_cmd));
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000); // 等待 5 秒後重試
    }
  }
}

// --- 動作執行函式 ---

void triggerBoot()
{
  if (digitalRead(checkAccPin) == HIGH)
    return;
  is_acting = true;
  Serial.println("引擎啟動");
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(400);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  delay(300);
  digitalWrite(RELAY_PIN_BOOT, LOW);
  delay(3000);
  digitalWrite(RELAY_PIN_BOOT, HIGH);
  digitalWrite(POWER_PIN, LOW);
  is_acting = false;
}

void unlockDoor()
{
  if (digitalRead(checkAccPin) == HIGH)
    return;
  is_acting = true;
  Serial.println("open door..");
  preAct = 1;
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_OPEN, LOW);
  delay(200);
  digitalWrite(RELAY_PIN_OPEN, HIGH);
  delay(100);
  digitalWrite(POWER_PIN, LOW);
  SendCarPowerMsg(3);
  is_acting = false;
}

void lockDoor()
{
  // if (digitalRead(checkAccPin) == HIGH || digitalRead(checkBluePin) == HIGH)
  //   return;
  if (digitalRead(checkAccPin) == HIGH)
    return;
  is_acting = true;
  Serial.println("lock door");
  preAct = 0;
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(200);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  delay(100);
  digitalWrite(POWER_PIN, LOW);
  SendCarPowerMsg(2);
  is_acting = false;
}

void closeWindow() // 關窗
{
  if (digitalRead(checkAccPin) == HIGH)
    return;
  is_acting = true;
  Serial.println("close window");
  preAct = 0;
  //按一下鎖門 放開後再按鎖門三秒
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(200);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  delay(1000);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(5000);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  delay(200);
  digitalWrite(POWER_PIN, LOW);
  SendCarPowerMsg(4);
  is_acting = false;
}

void OpenWindow() // 開窗
{
  if (digitalRead(checkAccPin) == HIGH)
    return;
  is_acting = true;
  Serial.println("open window");
  preAct = 0;
  // 按一下開門 放開後再按開門2秒
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_OPEN, LOW);
  delay(200);
  digitalWrite(RELAY_PIN_OPEN, HIGH);
  delay(1000);
  digitalWrite(RELAY_PIN_OPEN, LOW);
  delay(3000);
  digitalWrite(RELAY_PIN_OPEN, HIGH);
  delay(200);
  digitalWrite(POWER_PIN, LOW);
  SendCarPowerMsg(5);
  is_acting = false;
}

// 保留原 HTTP 發送功能 (只負責上報狀態，不負責接收命令)
void SendCarPowerMsg(int sts)
{
  String url = "";  // 初始化為空字串
  
  // if (sts == 1)
  //   // url = URL_CAR_BOOT;
  //   url = "";
  if (sts == 0)
    url = URL_CAR_SHUTDOWN;
  else if (sts == 2)
    url = URL_LOCK_DOOR;
  else if (sts == 3)
    url = URL_OPEN_DOOR;
  else if (sts == 4)
    url = URL_CLOSE_WINDOW;
  else if (sts == 5)
    url = URL_OPEN_WINDOW;
  else
    return;  // 無效的狀態值，直接返回

  if (WiFi.status() == WL_CONNECTED && url.length() > 0)
  {
    HTTPClient http;
    WiFiClient wclient;
    http.begin(wclient, url);
    http.setTimeout(5000);  // 設定 5 秒超時
    int httpCode = http.GET();
    http.end();
    wclient.stop();  // 明確關閉連接
  }
}