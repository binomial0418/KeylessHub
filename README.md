# Keyless Hub (keyless-hub)

## Keyless Hub

## 專案概述

此專案 Keyless Hub（keyless-hub）為基於 ESP32 的智能車載控制系統，透過 MQTT 協議實現遠程車門解鎖/上鎖、發動控制，同時支援本地藍牙和傳感器檢測。系統採用 Modem Sleep 模式以降低功耗。

## 主要功能

### 核心功能
- **MQTT 遠程控制**：透過 OwnTracks/Command 頻道接收指令
  - `boot`：遠程發動引擎
  - `lock`：遠程鎖門
  - `unlock`：遠程開門

- **本地智能感測**：
  - ACC 狀態監測（發動/熄火檢測）
  - 藍牙連接狀態檢測
  - 人體/敲擊傳感器檢測

- **自動化邏輯**：
  - 藍牙連接 + 人體靠近時自動開門
  - 藍牙斷開時 5 秒後自動鎖門
  - ACC 狀態變化自動上報

- **省電模式**：WiFi Modem Sleep + MQTT KeepAlive 機制

## 硬體配置

| 功能 | GPIO 腳位 | 說明 |
|------|---------|------|
| 藍牙偵測 | GPIO 34 | 手機藍牙連接狀態 |
| ACC 偵測 | GPIO 35 | 汽車點火狀態 |
| 人體偵測 | GPIO 33 | 人體/敲擊傳感器 |
| 門鎖繼電器 | GPIO 5 | 控制車門上鎖 |
| 發車繼電器 | GPIO 4 | 控制引擎啟動 |
| 開門繼電器 | GPIO 15 | 控制車門解鎖 |
| 鑰匙電源 | GPIO 26 | 電源控制 |
| 備用 | GPIO 27 | 預留接口 |

## 軟體架構

### 主要模組

#### `config.h` - 設定檔
集中管理所有系統設定：
- **MQTT 配置**：伺服器地址、埠號、帳號密碼
- **OwnTracks 配置**：使用者 ID、設備 ID
- **Wi-Fi 配置**：SSID 和密碼
- **上報 URL**：四個狀態上報端點

#### `car_cmd_v9.ino` - 主程式
包含以下主要函式：

| 函式 | 說明 |
|------|------|
| `setup()` | 系統初始化，GPIO 配置、WiFi 連接、MQTT 訂閱 |
| `loop()` | 主循環，MQTT 連線維護、GPIO 狀態檢查 |
| `callback()` | MQTT 訊息接收處理 |
| `checkPinStates()` | GPIO 狀態檢查與邏輯處理 |
| `checkOpenDoor()` | 開門條件驗證（3 秒內確認） |
| `setup_wifi()` | WiFi 連接與 Modem Sleep 啟用 |
| `reconnect()` | MQTT 斷線重連機制 |
| `triggerBoot()` | 執行發動流程 |
| `unlockDoor()` | 執行開門流程 |
| `lockDoor()` | 執行鎖門流程 |
| `SendCarPowerMsg()` | 上報狀態至伺服器 |

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
│  • <act>boot</act>   → 執行發動                             │
│  • <act>lock</act>   → 執行鎖門                             │
│  • <act>unlock</act> → 執行開門                             │
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
arduino-cli compile --fqbn esp32:esp32:esp32 /path/to/car_cmd_v9
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
   arduino-cli upload -p /dev/cu.usbserial-110  --fqbn esp32:esp32:esp32:UploadSpeed=460800 car_cmd.ino
   ```

### 完整工作流範例

```bash
# 1. 進入專案目錄
cd ~/Arduino/car_cmd_v9

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
- **ACC 監測**：檢測汽車點火狀態變化
- **去抖動**：ACC 變化後延遲 50ms 再確認，防止誤觸發
- **開門邏輯**：需同時滿足藍牙連接、ACC 關閉、人體靠近（3 秒內）
- **鎖門邏輯**：藍牙斷開後等待 5 秒，再次確認藍牙離開後執行

### 繼電器控制
- 所有繼電器高電位為關閉，低電位為啟動
- 每個動作完成後恢復初始狀態
- 發動流程：點火 → 鎖→解鎖 → 啟動馬達 → 停止 → 熄火

## 故障排查

| 問題 | 可能原因 | 解決方案 |
|------|--------|--------|
| 無法連接 WiFi | SSID/密碼錯誤 | 檢查 `config.h` 中的 WiFi 設定 |
| 無法連接 MQTT | 帳號密碼錯誤或伺服器離線 | 驗證 MQTT 帳號密碼和伺服器地址 |
| 收不到指令 | 未訂閱正確的頻道 | 檢查 OwnTracks 使用者 ID 和設備 ID |
| 繼電器無反應 | GPIO 連接問題 | 檢查硬體接線和 GPIO 定義 |
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
   - `1.html` - 流程圖視覺化
   - 建議 Git 倉庫名稱：`keyless-hub`

## 版本歷史

### v9（當前版本）
- ✅ 集成 MQTT 作為主要控制方式
- ✅ 提取配置到獨立 `config.h`
- ✅ 實現 Modem Sleep 省電模式
- ✅ MQTT 防斷線機制（KeepAlive）
- ✅ 完整的 GPIO 狀態檢查邏輯
- ✅ 自動化開門/鎖門邏輯
