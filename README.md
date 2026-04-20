# Keyless Hub (keyless-hub)

## Keyless Hub

## 專案概述

此專案 Keyless Hub（keyless-hub）為基於 ESP32 的智能車載控制系統，透過 MQTT 協議實現遠程車門解鎖/上鎖、發動控制，同時支援本地藍牙和傳感器檢測。系統採用 Modem Sleep 模式以降低功耗。

起因：原本汽車已經可以使用遙控鑰匙遠端發動，但只侷限於鑰匙訊號打的到汽車的地方，這樣有點失去遠端發車的美意(夏天先吹冷氣降溫)。因此本專案藉著esp32來遠端操作晶片鑰匙這樣來達到 『真 遠端發車』，如此就能涼涼的上車。之後就一路擴展功能，加入了車輛數據顯示，油耗計算，文件相關等功能，最後變成了手機鑰匙。

## 主要功能

### 核心功能
- **MQTT 遠程控制**：透過 OwnTracks/Command 頻道接收指令
  - `boot`：遠程發動引擎
  - `lock`：遠程鎖門
  - `unlock`：遠程開門
  - `window_close`：遠程關車窗
  - `window_open`：遠程開車窗
  - `key_on`：啟用鑰匙連結，實現真正免帶鑰匙
  - `key_off`：停用鑰匙連結
  
- **OBD 數據轉發**：
  - **WebSocket Server**：開啟於 Port 81，接收本地數據輸入。
  - **MQTT 轉發**：將收到的數據轉發至獨立的 OBD MQTT 頻道（owntracks/mt/obd）。
  - **獨立運作**：使用獨立的 MQTT 客戶端，確保數據傳輸不影響核心車控功能。
- **本地智能感測**：
  - ACC 狀態監測（發動/熄火檢測）
  - 藍牙連接狀態檢測
  - 人體/敲擊傳感器檢測

- **自動化邏輯**：
  - **自動開門**：藍牙連接 + 人體靠近（GPIO 33 觸發）時自動開門。
  - **自動鎖門**：藍牙斷開時，延遲 5 秒後自動鎖門。
  - **鑰匙電源控制**：藍牙連接且 `key_link` 啟用時，自動開啟鑰匙電源（POWER_PIN）。
  - **電池通電**：藍牙連接時，自動開啟備用電源（R1_PIN）。

- **省電模式**：WiFi Modem Sleep + MQTT KeepAlive 機制，降低待機功耗。

## 硬體配置

| 功能 | GPIO 腳位 | 說明 |
|------|---------|------|
| 藍牙偵測 | GPIO 34 | 手機藍牙連接狀態 (High: 已連接) |
| ACC 偵測 | GPIO 35 | 汽車點火狀態 (High: 發動中) |
| 人體偵測 | GPIO 33 | 人體/敲擊傳感器 (High: 觸發) |
| 門鎖繼電器 | GPIO 18 | 控制車門上鎖 |
| 發車繼電器 | GPIO 19 | 控制引擎啟動 |
| 開門繼電器 | GPIO 23 | 控制車門解鎖 |
| 鑰匙電源 | GPIO 26 | 控制晶片鑰匙電源 (POWER_PIN) |
| 備用電源 | GPIO 27 | 藍牙連線時通電 (R1_PIN) |

## 軟體架構

### 主要模組

#### `config.h` - 設定檔
集中管理所有系統設定：
- **MQTT 配置**：伺服器地址、埠號、帳號密碼
- **OwnTracks 配置**：使用者 ID、設備 ID
- **Wi-Fi 配置**：SSID 和密碼
- **上報 URL**：包含發動、熄火、鎖門、開門、關窗、開窗等狀態上報端點

#### `keyless-hub.ino` - 主程式
包含以下主要函式：

| 函式 | 說明 |
|------|------|
| `setup()` | 系統初始化，GPIO 配置、WiFi 連接、MQTT 訂閱、啟用 Modem Sleep |
| `loop()` | 主循環，MQTT 連線維護、GPIO 狀態檢查 |
| `callback()` | MQTT 訊息接收處理，解析 `boot`, `lock`, `unlock`, `key_on/off`, `window_open/close` |
| `checkPinStates()` | GPIO 狀態檢查與邏輯處理（ACC 變化、藍牙狀態、鑰匙電源控制） |
| `checkOpenDoor()` | 開門條件驗證（3 秒內確認人體感應） |
| `setup_wifi()` | WiFi 連接與 Modem Sleep 啟用 (`WiFi.setSleep(true)`) |
| `reconnect()` | MQTT 斷線重連機制，包含 WiFi 狀態檢查 |
| `triggerBoot()` | 執行發動流程（包含電源切換與繼電器控制） |
| `unlockDoor()` | 執行開門流程 |
| `lockDoor()` | 執行鎖門流程 |
| `closeWindow()` | 執行關窗流程（模擬長按鎖門） |
| `OpenWindow()` | 執行開窗流程（模擬長按開門） |
| `SendCarPowerMsg()` | 透過 HTTP GET 上報狀態至伺服器 |
| `reconnectOBD()` | OBD MQTT 斷線重連機制（每 5 秒嘗試一次，非阻塞） |
| `webSocketEvent()` | 處理 WebSocket 連線、斷線及收到數據後的 MQTT 轉發 |

## 程式運作流程

```
┌─────────────────────────────────────────────────────────────┐
│                    系統初始化 (Setup)                         │
│  GPIO → WiFi → MQTT → 訂閱 owntracks/{USER_ID}/{DEVICE_ID}   │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│                     主循環 (Loop)                            │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ 1. MQTT 連線維護 (reconnect + client.loop)            │    │
│  └──────────────────────────────────────────────────────┘   │
│                           ↓                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ 2. GPIO 狀態檢查 (checkPinStates)                     │   │
│  │    • ACC 變化檢測 → 上報狀態                            │   │
│  │    • 藍牙狀態 → 控制 R1_PIN (電池通電)                  │   │
│  │    • 藍牙 + key_link → 控制 POWER_PIN (鑰匙電源)        │   │
│  │    • 開門邏輯：藍牙 + 人體靠近                           │   │
│  │    • 鎖門邏輯：藍牙斷開後 5 秒                           │   │
│  └──────────────────────────────────────────────────────┘   │
│                           ↓                                 │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ 3. 延遲 10ms (Modem Sleep 省電)                       │   │
│  └──────────────────────────────────────────────────────┘   │
│                           ↓                                 │
│                      [循環重複]                              │
└─────────────────────────────────────────────────────────────┘

         ↙ 非同步 ↙
┌────────────────────────────────────────────────────────────┐
│              接收 MQTT 指令 (Callback)                      │
│  • boot         → 執行發動                                  │
│  • lock         → 執行鎖門                                  │
│  • unlock       → 執行開門                                  │
│  • key_on/off   → 切換鑰匙連結狀態                          │
│  • window_open  → 執行開窗                                  │
│  • window_close → 執行關窗                                  │
└────────────────────────────────────────────────────────────┘
```

## 詳細流程圖

![Keyless Hub 流程圖](flow.html)

> 點擊上方 HTML 檔案查看完整流程圖

## 快速開始

### 設定步驟

1. **建立設定檔**
   ```bash
   cp config.h.example config.h
   ```

2. **編輯 `config.h`** 根據您的環境設定以下參數：

#### MQTT 伺服器設定
```cpp
#define MQTT_HOST ""      // MQTT 伺服器地址
#define MQTT_PORT 50883   // MQTT 埠號
#define MQTT_USER "user"  // MQTT 使用者名稱
#define MQTT_PASS "pass"  // MQTT 密碼
```

#### Wi-Fi 設定
```cpp
#define WIFI_SSID "wifi"         // Wi-Fi SSID
#define WIFI_PASSWORD "pass"     // Wi-Fi 密碼
```

#### OwnTracks 使用者資訊
```cpp
#define USER_ID "id"                   // OwnTracks 使用者 ID
#define DEVICE_ID "device"                // OwnTracks 設備 ID
// 訂閱頻道: owntracks/{USER_ID}/{DEVICE_ID}
```

#### 上報狀態 URL
```cpp
#define URL_CAR_BOOT "http://..."      // 汽車發動
#define URL_CAR_SHUTDOWN "http://..."  // 汽車關閉
#define URL_LOCK_DOOR "http://..."     // 鎖門
#define URL_OPEN_DOOR "http://..."     // 開門
#define URL_CLOSE_WINDOW "http://..."  // 關窗
#define URL_OPEN_WINDOW "http://..."   // 開窗
```


這樣可以防止敏感的設定資訊（如密碼）洩露到版本控制系統。使用者可以根據 `config.h.example` 建立自己的 `config.h`。

## 依賴庫

本專案使用以下 Arduino 庫：
- **WiFi**（ESP32 內建）
- **HTTPClient**（ESP32 內建）
- **base64**
- **PubSubClient**（用於 MQTT）

可使用 `arduino-cli` 查詢已安裝的庫：
```bash
arduino-cli lib list
```

## 編譯與上傳（使用 arduino-cli）

### 前置準備

1. **安裝 arduino-cli**（如尚未安裝）
   ```bash
   # macOS
   brew install arduino-cli
   
   # Linux
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
   ```

2. **初始化設定**
   ```bash
   arduino-cli config init
   ```

3. **安裝 ESP32 核心**
   ```bash
   arduino-cli core install esp32:esp32
   ```

4. **安裝必要的庫**
   ```bash
   arduino-cli lib install "PubSubClient"
   arduino-cli lib install "base64"
   ```

### 編譯

使用以下命令編譯程式碼：

```bash
# 編譯專案（需在專案目錄中運行）
arduino-cli compile --fqbn esp32:esp32:esp32 .

# 或指定完整路徑
arduino-cli compile --fqbn esp32:esp32:esp32 /path/to/keyless-hub
```

其中 `--fqbn esp32:esp32:esp32` 表示使用 ESP32 Dev Module 開發板。

### 上傳

1. **列出可用的串列埠**
   ```bash
   arduino-cli board list
   ```

2. **上傳程式碼到 ESP32**
   ```bash
   # 使用發現的串列埠上傳
   arduino-cli upload -p /dev/cu.usbserial-110  --fqbn esp32:esp32:esp32:UploadSpeed=460800 keyless-hub.ino
   ```

### 完整工作流範例

```bash
# 1. 進入專案目錄
cd ~/Arduino/keyless-hub

# 2. 確認設定檔已建立
cp config.h.example config.h
# 編輯 config.h 修改您的設定

# 3. 編譯
arduino-cli compile --fqbn esp32:esp32:esp32 .

# 4. 列出可用串列埠
arduino-cli board list

# 5. 上傳
arduino-cli upload -p /dev/cu.usbserial-14120 --fqbn esp32:esp32:esp32:UploadSpeed=460800 .

# 6. 查看串列輸出（波特率 115200）
arduino-cli monitor -p /dev/cu.usbserial-14120 -c baudrate=115200
```

### 常用 arduino-cli 命令

| 命令 | 說明 |
|------|------|
| `arduino-cli core list` | 列出已安裝的開發板核心 |
| `arduino-cli board list` | 列出連接的開發板 |
| `arduino-cli compile --fqbn <fqbn> <path>` | 編譯程式碼 |
| `arduino-cli upload -p <port> --fqbn <fqbn> <path>` | 上傳程式碼 |
| `arduino-cli monitor -p <port>` | 開啟串列監視器 |
| `arduino-cli lib search <keyword>` | 搜尋庫 |
| `arduino-cli lib install <library>` | 安裝庫 |
| `arduino-cli lib list` | 列出已安裝的庫 |

## 工作原理詳解

### 省電機制
- **WiFi Modem Sleep**：在 `setup_wifi()` 中執行 `WiFi.setSleep(true)`
- **MQTT KeepAlive**：設定 60 秒心跳，避免 NAT 斷線
- **短循環延遲**：Loop 中僅 10ms 延遲，CPU 空閒時 WiFi 自動進入省電模式

### MQTT 連線維護
- 連線成功後自動訂閱 `owntracks/mt/cmd` 頻道
- 如果 WiFi 斷開，自動重連 WiFi 再連 MQTT
- 若 MQTT 連線失敗，5 秒後重試

### GPIO 狀態檢查
- **ACC 監測**：檢測汽車點火狀態變化，並自動上報至伺服器。
- **去抖動**：ACC 變化後延遲 50ms 再確認，防止誤觸發。
- **藍牙連動**：
  - 當藍牙連接時，`R1_PIN` (GPIO 27) 會通電（電池通電）。
  - 當藍牙連接且 `key_link` 狀態為 1 時，`POWER_PIN` (GPIO 26) 會通電（鑰匙電源）。
- **開門邏輯**：需同時滿足藍牙連接、ACC 關閉、人體靠近（GPIO 33 在 3 秒內觸發）。
- **鎖門邏輯**：藍牙斷開後等待 5 秒，再次確認藍牙離開後執行鎖門動作。

### OBD 數據轉發機制
- **WebSocket 接收**：監聽 Port 81，接收來自本地裝置（如平板或手機）的原始 OBD 數據。
- **異步轉發**：所有接收到的數據會立即透過 `obdClient` 發布至雲端 MQTT，實現遠端監控。
- **穩定性**：具備獨立的連線維護機制 (`reconnectOBD`)，即使 OBD MQTT 斷線也補會影響主要車控邏輯。

### 繼電器控制
- 所有繼電器高電位為關閉，低電位為啟動。
- 每個動作完成後恢復初始狀態。
- **發動流程**：開啟鑰匙電源 → 鎖門脈衝 → 啟動馬達 (3秒) → 關閉鑰匙電源。
- **窗戶控制**：
  - **關窗**：模擬「按一下鎖門」後「長按鎖門 5 秒」。
  - **開窗**：模擬「按一下開門」後「長按開門 3 秒」。

## 故障排查

| 問題 | 可能原因 | 解決方案 |
|------|--------|--------|
| 無法連接 WiFi | SSID/密碼錯誤 | 檢查 `config.h` 中的 WiFi 設定 |
| 無法連接 MQTT | 帳號密碼錯誤或伺服器離線 | 驗證 MQTT 帳號密碼和伺服器地址 |
| 收不到指令 | 未訂閱正確的頻道 | 檢查 OwnTracks 使用者 ID 和設備 ID |
| 繼電器無反應 | GPIO 連接問題 | 檢查硬體接線和 GPIO 定義 |
| 鑰匙電源不通 | 藍牙未連或 `key_link` 未開啟 | 確認藍牙狀態，並發送 `key_on` 指令 |
| 功耗過高 | Modem Sleep 未啟用 | 確認 `WiFi.setSleep(true)` 已執行 |

## 日誌輸出

系統透過串列埠輸出調試訊息（波特率 115200）：
```
系統啟動完成，進入 Modem Sleep 監聽模式
Connecting to <YOUR_WIFI_SSID>
WiFi 連接成功
IP: <YOUR_IP_ADDRESS>
Modem Sleep 已啟用
Attempting MQTT connection...connected
已訂閱頻道: owntracks/<USER_ID>/<DEVICE_ID>
```

## 作者與授權

- **開發目的**：智能車載控制系統
- **最後更新**：2025年12月
- **相關文件**：
   - `config.h` - 系統設定
   - `flow.html` - 流程圖視覺化
   - 建議 Git 倉庫名稱：`keyless-hub`

## 版本歷史

### v9（當前版本）
- ✅ 集成 MQTT 作為主要控制方式
- ✅ 提取配置到獨立 `config.h`
- ✅ 實現 Modem Sleep 省電模式
- ✅ MQTT 防斷線機制（KeepAlive）
- ✅ 關鍵動作流程完成
- ✅ 自動化開門/鎖門邏輯

### v10（當前版本）
- ✅ 新增 WebSocket Server (Port 81)
- ✅ 實現獨立的 OBD 數據 MQTT 轉發機制
- ✅ 強化 MQTT 連線穩定性與非阻塞重連
