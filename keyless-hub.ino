#include "config.h"
#include <Arduino.h>
#include <ElegantOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h> // 請確認已安裝 PubSubClient Library
#include <WebServer.h>
#include <WebSocketsServer.h> // 請確認已安裝 WebSockets Library
#include <WiFi.h>
#include <base64.h>

// ====== 硬體腳位設定 (保留您的原始設定) ======
#define checkBluePin 34   // 手機藍牙偵測
#define checkAccPin 35    // 汽車 ACC 偵測
#define RELAY_PIN_LOCK 5  // 鎖門
#define RELAY_PIN_BOOT 4  // 發車
#define RELAY_PIN_OPEN 15 // 開門
#define POWER_PIN 26      // 鑰匙電源
#define R1_PIN 27         // 備用

// ====== 全域變數 ======
int preAct = 0;
int lastAccState = -1;   // 用來偵測 ACC 狀態變化的變數
int lastBlueState = -1;  // 用來偵測藍牙狀態變化的變數（debounce 用）
int key_link = 0;       // MQTT 控制連動
bool is_acting = false;          // 是否正在執行動作
bool autoUnlockTriggered = false; // 本次連線是否已自動解鎖過

// --- 新增設定變數 ---
Preferences preferences;
String pref_ssid = WIFI_SSID;
String pref_pass = WIFI_PASSWORD;
String pref_mqtt_host = MQTT_HOST;
int pref_mqtt_port = MQTT_PORT;
String pref_mqtt_user = MQTT_USER;
String pref_mqtt_pass = MQTT_PASS;
String pref_user_id = USER_ID;
String pref_device_id = DEVICE_ID;

// ====== OwnTracks/Command 設定 ======
String topic_cmd = "owntracks/" + pref_user_id + "/" + pref_device_id;

int open_window_delay = 3000;
int close_window_delay = 5000;

// --- AP 模式變數 ---
bool ap_active = false;
// 移除 AP_TIMEOUT，改為永久開啟

// MQTT 物件
WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

// --- OBD 擴充物件 ---
WiFiClient obdEspClient;
PubSubClient obdClient(obdEspClient);
WebSocketsServer webSocket = WebSocketsServer(81);

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

// --- OBD 擴充功能 ---
void reconnectOBD();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length);

// --- 新增函式宣告 ---
void startAP();
void handleRoot();
void handleSave();
void loadSettings();
void saveSettings();

void setup() {
  Serial.begin(115200);

  // --- 載入設定 ---
  loadSettings();

  // --- 啟動 AP 模式 (10分鐘) ---
  startAP();

  // --- 腳位設定 ---
  pinMode(checkBluePin, INPUT);
  pinMode(checkAccPin, INPUT);
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

  // --- 連接 WiFi ---
  setup_wifi();

  // --- 設定 MQTT ---
  client.setServer(pref_mqtt_host.c_str(), pref_mqtt_port);
  client.setCallback(callback); // 設定收到訊息的處理函式

  // --- 設定 OBD MQTT ---
  obdClient.setServer(OBD_MQTT_HOST, OBD_MQTT_PORT);

  // --- 設定 WebSocket ---
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // --- [修改 4] MQTT 防斷線機制: KeepAlive ---
  // 設定 60 秒發送一次心跳，避免 4G NAT 斷線
  client.setKeepAlive(60);

  Serial.println("系統啟動完成，進入 Modem Sleep 監聽模式");
}

void loop() {
  // --- AP 模式處理 (常駐) ---
  if (ap_active) {
    server.handleClient();
  }

  // --- WebSocket 處理 ---
  webSocket.loop();

  // --- MQTT 連線維護 ---
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // 這裡非常重要！它負責處理 MQTT 訊息接收與心跳包

  // --- OBD MQTT 連線維護 ---
  if (!obdClient.connected()) {
    reconnectOBD();
  }
  obdClient.loop();

  // ---  處理腳位邏輯 ---
  checkPinStates();
  // Serial.print("POWER_PIN state: ");
  // Serial.println(digitalRead(POWER_PIN));
  // 這裡的 delay 雖然短，但在 Modem Sleep 模式下，
  // 只要 CPU 空閒，WiFi 模組就會自動尋找機會關閉射頻省電。
  delay(10);
}

// ------------------ 邏輯功能區 ------------------

// [修改 2] 處理 MQTT 收到的訊息 (取代原本 bootCar 的 HTTP GET)
void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("收到指令 [");
  Serial.print(topic);
  Serial.print("]: ");

  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.println(msg);

  // 解析mqtt指令
  if (msg.indexOf("boot") != -1) {
    triggerBoot();
  } else if (msg.indexOf("lock") != -1 && msg.indexOf("unlock") == -1) {
    lockDoor();
  } else if (msg.indexOf("unlock") != -1) {
    unlockDoor();
  } else if (msg.indexOf("key_on") != -1) {
    key_link = 1;
    Serial.println("key_link set to 1");
  } else if (msg.indexOf("key_off") != -1) {
    key_link = 0;
    Serial.println("key_link set to 0");
  } else if (msg.indexOf("window_open") != -1) {
    OpenWindow();
  } else if (msg.indexOf("window_close") != -1) {
    closeWindow();
  } else if (msg.indexOf("wake-ap") != -1) {
    startAP();
  }
}

// 綜合狀態檢查函式 (在 loop 中執行)
void checkPinStates() {
  int currentAcc = digitalRead(checkAccPin);
  int currentBlue = digitalRead(checkBluePin);
  // 藍牙連上時，電池通電
  if (currentBlue == HIGH) {
    digitalWrite(R1_PIN, HIGH);
  } else {
    digitalWrite(R1_PIN, LOW);
  }

  // --- 藍牙 HIGH→LOW 邊緣偵測（含 debounce）---
  if (currentBlue != lastBlueState) {
    delay(50); // 去抖動
    if (digitalRead(checkBluePin) == currentBlue) {
      // 確認為 HIGH→LOW 真實斷線
      if (lastBlueState == HIGH && currentBlue == LOW) {
        Serial.println("藍牙斷線確認");
        if (autoUnlockTriggered) {
          Serial.println("藍牙斷線，自動鎖門...");
          lockDoor();
          preAct = 0;
          autoUnlockTriggered = false;
        }
      }
      lastBlueState = currentBlue;
    }
  }

  // --- [新增] Power Pin 控制邏輯 ---
  if (!is_acting) {
    if (currentBlue == HIGH && key_link == 1) {
      // 當連結key and acc off and 門沒開時，自動解鎖
      if (preAct == 0 && currentAcc != HIGH && !autoUnlockTriggered) {
        unlockDoor();
        autoUnlockTriggered = true;
        // 避免連續觸發
        delay(2000);
      }
      digitalWrite(POWER_PIN, HIGH);
    } else {
      if (key_link == 0) {
        if (autoUnlockTriggered) {
          preAct = 0;
        }
        autoUnlockTriggered = false;
      }
      digitalWrite(POWER_PIN, LOW);
    }
  }

  // --- 1. 偵測 ACC 狀態變化 ---
  if (currentAcc != lastAccState) {
    delay(50); // 去抖動
    if (digitalRead(checkAccPin) == currentAcc) {
      if (currentAcc == LOW) {
        // ACC 從 ON 變 OFF
        Serial.println("汽車關閉 (ACC OFF)");
        SendCarPowerMsg(0);
      } else {
        // ACC 從 OFF 變 ON
        Serial.println("汽車發動 (ACC ON)");
      }
      lastAccState = currentAcc;
    }
  }

}

void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(pref_ssid);

  WiFi.mode(WIFI_AP_STA); // 設定為 AP+Station 模式，以便同時運行 AP
  WiFi.begin(pref_ssid.c_str(), pref_pass.c_str());

  int iCount = 0;
  while (WiFi.status() != WL_CONNECTED && iCount <= 20) {
    delay(500);
    Serial.print(".");
    iCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 連接成功");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // ====== [修改 1] 這裡就是 Modem Sleep 的開關！ ======
    // 當 Wi-Fi 連線成功後，明確告訴 ESP32 開啟省電模式。
    // 這行執行後，底層會在沒有資料傳輸時，自動關閉 Wi-Fi 射頻。
    WiFi.setSleep(true);
    Serial.println("Modem Sleep 已啟用");
    // =================================================
  } else {
    Serial.println("\nWiFi 連接失敗，將於 loop 中重試");
  }
}

// [修改 4] MQTT 斷線重連機制
void reconnect() {
  // 如果 WiFi 斷了，先重連 WiFi
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }

  // 檢查 MQTT 是否連線
  if (!client.connected()) {
    Serial.print("Attempting MQTT connection...");

    // 產生隨機 Client ID
    String clientId = "ESP32-Car-" + String(random(0xffff), HEX);

    // 使用設定的帳號密碼連線
    if (client.connect(clientId.c_str(), pref_mqtt_user.c_str(),
                       pref_mqtt_pass.c_str())) {
      Serial.println("connected");

      // 連線成功後，重新訂閱命令頻道
      client.subscribe(topic_cmd.c_str());
      Serial.println("已訂閱頻道: " + topic_cmd);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000); // 等待 5 秒後重試
    }
  }
}

// --- 動作執行函式 ---

void triggerBoot() {
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

void unlockDoor() {
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

void lockDoor() {
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
  // 按一下鎖門 放開後再按鎖門三秒
  digitalWrite(POWER_PIN, HIGH);
  delay(100);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(200);
  digitalWrite(RELAY_PIN_LOCK, HIGH);
  delay(1000);
  digitalWrite(RELAY_PIN_LOCK, LOW);
  delay(close_window_delay);
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
  delay(open_window_delay);
  digitalWrite(RELAY_PIN_OPEN, HIGH);
  delay(200);
  digitalWrite(POWER_PIN, LOW);
  SendCarPowerMsg(5);
  is_acting = false;
}

// 保留原 HTTP 發送功能 (只負責上報狀態，不負責接收命令)
void SendCarPowerMsg(int sts) {
  String url = ""; // 初始化為空字串

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
    return; // 無效的狀態值，直接返回

  if (WiFi.status() == WL_CONNECTED && url.length() > 0) {
    HTTPClient http;
    WiFiClient wclient;
    http.begin(wclient, url);
    http.setTimeout(5000); // 設定 5 秒超時
    int httpCode = http.GET();
    http.end();
    wclient.stop(); // 明確關閉連接
  }
}

// ------------------ 設定與 AP 模式 ------------------

void loadSettings() {
  preferences.begin("settings", true);
  pref_ssid = preferences.getString("ssid", WIFI_SSID);
  pref_pass = preferences.getString("pass", WIFI_PASSWORD);
  pref_mqtt_host = preferences.getString("m_host", MQTT_HOST);
  pref_mqtt_port = preferences.getInt("m_port", MQTT_PORT);
  pref_mqtt_user = preferences.getString("m_user", MQTT_USER);
  pref_mqtt_pass = preferences.getString("m_pass", MQTT_PASS);
  pref_user_id = preferences.getString("u_id", USER_ID);
  pref_device_id = preferences.getString("d_id", DEVICE_ID);
  open_window_delay = preferences.getInt("ow_delay", 3000);
  close_window_delay = preferences.getInt("cw_delay", 5000);
  preferences.end();

  // 更新 MQTT Topic
  topic_cmd = "owntracks/" + pref_user_id + "/" + pref_device_id;
}

void saveSettings() {
  preferences.begin("settings", false);
  preferences.putString("ssid", pref_ssid);
  preferences.putString("pass", pref_pass);
  preferences.putString("m_host", pref_mqtt_host);
  preferences.putInt("m_port", pref_mqtt_port);
  preferences.putString("m_user", pref_mqtt_user);
  preferences.putString("m_pass", pref_mqtt_pass);
  preferences.putString("u_id", pref_user_id);
  preferences.putString("d_id", pref_device_id);
  preferences.putInt("ow_delay", open_window_delay);
  preferences.putInt("cw_delay", close_window_delay);
  preferences.end();
}

void handleRoot() {
  String html =
      "<html><head><meta charset='UTF-8'><title>Keyless Hub 設定</title>";
  html += "<style>body{font-family:sans-serif;margin:20px;} "
          "input{margin-bottom:10px;width:100%;padding:8px;} "
          "button{padding:10px;width:100%;background:#007bff;color:white;"
          "border:none;cursor:pointer;}</style>";
  html += "</head><body>";
  html += "<h1>Keyless Hub 設定</h1>";
  html += "<form action='/save' method='POST'>";
  html += "<h3>Wi-Fi 設定</h3>";
  html += "SSID: <input type='text' name='ssid' value='" + pref_ssid + "'><br>";
  html +=
      "密碼: <input type='password' name='pass' value='" + pref_pass + "'><br>";

  html += "<h3>MQTT 設定</h3>";
  html += "Host: <input type='text' name='m_host' value='" + pref_mqtt_host +
          "'><br>";
  html += "Port: <input type='number' name='m_port' value='" +
          String(pref_mqtt_port) + "'><br>";
  html += "User: <input type='text' name='m_user' value='" + pref_mqtt_user +
          "'><br>";
  html += "Pass: <input type='password' name='m_pass' value='" +
          pref_mqtt_pass + "'><br>";
  html += "User ID: <input type='text' name='u_id' value='" + pref_user_id +
          "'><br>";
  html += "Device ID: <input type='text' name='d_id' value='" + pref_device_id +
          "'><br>";

  html += "<h3>延時設定 (毫秒)</h3>";
  html += "開窗延時: <input type='number' name='ow_delay' value='" +
          String(open_window_delay) + "'><br>";
  html += "關窗延時: <input type='number' name='cw_delay' value='" +
          String(close_window_delay) + "'><br>";

  html += "<button type='submit'>儲存並重啟</button>";
  html += "</form>";
  html += "<br><hr><h3>韌體更新 (OTA)</h3>";
  html += "<p><a href='/update'>前往 ElegantOTA 更新頁面</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid"))
    pref_ssid = server.arg("ssid");
  if (server.hasArg("pass"))
    pref_pass = server.arg("pass");
  if (server.hasArg("m_host"))
    pref_mqtt_host = server.arg("m_host");
  if (server.hasArg("m_port"))
    pref_mqtt_port = server.arg("m_port").toInt();
  if (server.hasArg("m_user"))
    pref_mqtt_user = server.arg("m_user");
  if (server.hasArg("m_pass"))
    pref_mqtt_pass = server.arg("m_pass");
  if (server.hasArg("u_id"))
    pref_user_id = server.arg("u_id");
  if (server.hasArg("d_id"))
    pref_device_id = server.arg("d_id");
  if (server.hasArg("ow_delay"))
    open_window_delay = server.arg("ow_delay").toInt();
  if (server.hasArg("cw_delay"))
    close_window_delay = server.arg("cw_delay").toInt();

  saveSettings();
  server.send(200, "text/html",
              "<html><body><h1>設定已儲存，系統即將重啟...</"
              "h1><script>setTimeout(function(){location.href='/';}, "
              "3000);</script></body></html>");
  delay(2000);
  ESP.restart();
}

void startAP() {
  Serial.println("啟動 AP 模式...");
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP 位址: ");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  ElegantOTA.begin(&server);
  server.begin();

  ap_active = true;
}

// ================== OBD 擴充功能實作 ==================

void reconnectOBD() {
  static unsigned long lastRetry = 0;
  if (millis() - lastRetry < 5000)
    return; // 每 5 秒嘗試一次，非阻塞
  lastRetry = millis();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("嘗試連接 OBD MQTT...");
    String clientId = "ESP32-OBD-" + String(random(0xffff), HEX);
    if (obdClient.connect(clientId.c_str(), OBD_MQTT_USER, OBD_MQTT_PASS)) {
      Serial.println("OBD MQTT 已連線");
    } else {
      Serial.print("OBD MQTT 連線失敗, rc=");
      Serial.println(obdClient.state());
    }
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload,
                    size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] WebSocket 中斷連線\n", num);
    break;
  case WStype_CONNECTED: {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[%u] WebSocket 已連線，來自 %s\n", num,
                  ip.toString().c_str());
    break;
  }
  case WStype_TEXT:
    Serial.printf("[%u] 收到 WebSocket 數據: %s\n", num, payload);
    // 將收到的文字轉發至 OBD MQTT Topic
    if (obdClient.connected()) {
      obdClient.publish(OBD_MQTT_TOPIC, (const char *)payload);
      Serial.println("數據已轉發至 OBD MQTT");
    } else {
      Serial.println("OBD MQTT 未連線，無法轉發數據");
    }
    break;
  default:
    break;
  }
}