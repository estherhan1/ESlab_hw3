# HW-Lab3: BLE Central (GATT client)

| File | Purpose |
|---|---|
| `ble_central.py` | Basic problem: Python BLE central (bluepy) that scans, connects, dumps the GATT table, reads/writes a characteristic, **sets the CCCD to 0x0002**, and receives indications |
| `gattlib/build_gattlib.sh` | Optional problem 1: builds GATTLIB and its C examples on the RPi |

## 1. Setup on the Raspberry Pi

```bash
sudo apt install python3-pip libglib2.0-dev
sudo pip install bluepy          # newer Raspberry Pi OS: add --break-system-packages
sudo hciconfig hci0 up
```

Copy `ble_central.py` to the Pi (for example `scp ble_central.py pi@<rpi-ip>:~`).

## 2. Setup on the Android phone (BLE Tool)

1. Open **BLE Tool → GATT Server**, then press **Start Advertising**.
2. The default server has service `0xFFF0` with characteristic `0xFFF1`, which supports notify and indicate. If your app uses different UUIDs, pass `--service` / `--char`.
3. Keep the **server log** screen visible. It records connections, reads, writes and descriptor writes.

## 3. Run the basic problem

```bash
sudo python3 ble_central.py
```

1. The program scans for 10 seconds and lists the devices, strongest signal first. Enter the number of your phone. It usually has a **random** address and often shows the phone's name.
2. It prints every service, characteristic and descriptor. Look for `Desc 00002902-...`, which is the CCCD.
3. It reads `0xFFF1`.
4. It writes **`02 00`** (0x0002, little-endian) to the CCCD and reads the value back. The phone's server log should show a descriptor write of `0x0002` / "indication enabled".
5. For 30 seconds it prints any indication you send from the phone (press Notify/Indicate in BLE Tool).

Useful variants:

```bash
# skip scanning, write "efg" to the characteristic, then set CCCD
sudo python3 ble_central.py --addr XX:XX:XX:XX:XX:XX --write 656667
# enable notifications instead (0x0001) or disable (0x0000)
sudo python3 ble_central.py --addr XX:XX:XX:XX:XX:XX --cccd 0x0001
```

### iPhone + LightBlue (what this setup actually used)
iPhone 熱點下 Pi 的 IP 是 `172.20.10.4`（會變，用 `arp -a | grep b8:27:eb` 找）；bluepy 裝在 venv `~/ble`。
LightBlue 的 Blank 虛擬裝置廣播名稱為 `Blank`，其 service `0x1111` 內的
`b3fcaed7-8598-419f-89f9-ebf46229fc7d`（READ + INDICATE，CCCD handle 0x003e）就是目標：

```bash
ssh pi@172.20.10.4
sudo ~/ble/bin/python3 ble_central.py --service 1111 \
     --char b3fcaed7-8598-419f-89f9-ebf46229fc7d --cccd 0x0002 --wait 60
```
（iPhone 的 BLE 位址會變，所以不加 `--addr`，讓程式掃描後選 `name=Blank` 那台。）

### What to submit as proof
- A screenshot of the terminal output showing `Wrote CCCD = 0x0002` and `CCCD read back = 0200`.
- A screenshot of the BLE Tool **server log** showing the descriptor (0x2902) write with value `02 00`.
- Optionally, the indications received on the Pi (`<<< notification/indication ...`).

### Troubleshooting
- `Connection refused (111)`: the address type is wrong. Phones use `random`, so pass `--addr-type public` only for public-address devices.
- Nothing is found during the scan: make sure you are running with `sudo` and advertising is turned on in BLE Tool. Try `sudo hciconfig hci0 reset`.
- The phone's random address changes when you restart advertising, so re-scan instead of reusing an old `--addr`.
- Pairing prompts on the phone: accept them. They are not required for this lab.

## 4. Optional problem 1: GATTLIB (C)

GATTLIB 是 Linux 上的 C 語言 BLE 函式庫，底層透過 BlueZ 的 D-Bus API（`org.bluez`）運作。
這部分做了三件事：編譯安裝 GATTLIB、執行兩個官方範例、用 GATTLIB API 自己寫一個 C 程式。

| 檔案 | 說明 |
|---|---|
| `gattlib/build_gattlib.sh` | 下載、修正（libpcre → libpcre2-8）、編譯並安裝 GATTLIB 與官方範例 |
| `gattlib/my_adv_scan/my_adv_scan.c` | **自己寫的 C 程式**：BLE 掃描器，顯示名稱、RSSI、公司名稱、service UUID 與統計 |
| `gattlib/my_adv_scan/Makefile` | 用 `pkg-config gattlib` 編譯 my_adv_scan |
| `gattlib/company_name.py` | 輔助工具：把 `advertisement_data` 輸出的 company ID 轉成公司名稱 |
| `gattlib/gatt_names.py` | 輔助工具：替 `ble_scan` 輸出的 UUID 加名稱，並把 properties 翻成 READ/WRITE/NOTIFY/INDICATE |

### 4.1 編譯與安裝（在 Raspberry Pi 上）

```bash
cd gattlib && bash build_gattlib.sh
# 安裝到 /usr/local/lib/libgattlib.so 與 /usr/local/include/gattlib.h
```
Debian 13 (trixie) 已移除 PCRE1，官方範例的 CMake 找不到 `libpcre`；範例並沒有用到 PCRE，所以腳本把它改成 `libpcre2-8`。

輔助工具要用的 Bluetooth SIG 官方清單，下載一次放在 `~`：
```bash
curl -fsSL -o ~/company_identifiers.yaml https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml
B=https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/uuids
for f in service_uuids characteristic_uuids member_uuids; do curl -fsSL -o ~/$f.yaml $B/$f.yaml; done
cp gattlib/company_name.py gattlib/gatt_names.py ~
```

### 4.2 範例 1：advertisement_data（官方）
只聽廣播、不連線，印出每台裝置的 manufacturer data。
```bash
cd ~/gattlib-src/build/examples
sudo timeout 20 stdbuf -oL ./advertisement_data/advertisement_data | python3 ~/company_name.py
```
輸出例：`Device ... - 'Blank': - Manufacturer data for id 0x004c (Apple, Inc.): 10 07 ...`
（`stdbuf -oL` 讓輸出接到 pipe 時不被緩衝。）

### 4.3 範例 2：ble_scan（官方）
掃描 10 秒後逐一連線，用 `gattlib_discover_primary` / `gattlib_discover_char` 列出 services 與 characteristics。
```bash
sudo timeout 90 stdbuf -oL ./ble_scan/ble_scan | python3 ~/gatt_names.py
```
輸出例：`characteristic[2] properties:20 ... uuid:0x2a05  <- Service Changed [INDICATE]`。
人多的地方很多裝置連不上（`le-connection-abort-by-local`），所以用 `timeout` 限制時間；
`gattlib_connect()` 是非同步的，`DONE` 只代表請求已送出。

### 4.4 範例 3：my_adv_scan（自己寫的 C 程式）
```bash
cp -r gattlib/my_adv_scan ~ && cd ~/my_adv_scan
make
sudo ./my_adv_scan              # 掃 15 秒，只列有名字的裝置（LightBlue 的 Blank 會出現）
sudo ./my_adv_scan -t 10 -a     # 掃 10 秒，列出全部裝置
sudo ./my_adv_scan -h           # 參數：-t 秒數、-a 全部、-f 公司清單檔
```
輸出例：
```
[3] 70:31:7F:6A:5C:A1  'Blank'  RSSI -32 dBm
     Manufacturer 0x004C  Apple, Inc.                    data: 10 07 34 1f 3d ae 35 40 78
     Service UUID 0x1111
========== Summary ==========
  0x004C Apple, Inc.                        61
  0x0075 Samsung Electronics Co. Ltd.       1
```
用到的 GATTLIB API：`gattlib_mainloop` → `gattlib_adapter_open` → `gattlib_adapter_scan_enable_with_filter`
（callback 中 `gattlib_get_advertisement_data_from_mac`、`gattlib_get_rssi_from_mac`、`gattlib_uuid_to_string`）
→ `gattlib_adapter_scan_disable` / `gattlib_adapter_close`。
與官方範例相比：每台裝置只列一次（mutex 保護）、free 回傳的 buffer（官方範例沒有）、公司名稱、RSSI、統計與參數。

### 4.5 遇到的問題：GATTLIB 連 iPhone 需要配對
`discover`、`read_write`、`notification` 對 iPhone 都失敗。btmon 顯示：連線成功後 BlueZ 自動讀取 iOS 的 Apple service，
iPhone 回 `Insufficient Authentication (0x05)`，BlueZ 於是發起 LE Secure Connections 配對（數字比對）；
沒有 pairing agent 時逾時（D-Bus timeout `0x10010018`），用 agent 自動確認後 iPhone 端仍未完成配對，
`notification` 回報 characteristic not found，甚至 segfault。bluepy 直接送 ATT 封包、不讀加密 service，所以 Basic 題目不需要配對。

### 4.6 bluepy vs GATTLIB
| | bluepy (Python) | GATTLIB (C) |
|---|---|---|
| 底層 | bluepy-helper 直接走 HCI/ATT | BlueZ D-Bus API |
| 程式模型 | 同步呼叫 | 非同步：`gattlib_mainloop` + callback |
| CCCD | 自己寫 handle（`writeCharacteristic(0x3e, b"\x02\x00")`） | `gattlib_notification_start()` 由 BlueZ 代寫 |
| 對 iPhone | 可用，不需配對 | BlueZ 讀加密 service 觸發配對 |
