# HW-Lab3: BLE Central (GATT client)

Raspberry Pi 3 當 BLE central，iPhone 上的 LightBlue 虛擬裝置 **Blank** 當 peripheral。

| 檔案 | 用途 |
|---|---|
| `ble_central.py` | **Basic problem**：Python（bluepy）BLE central，負責掃描、連線、列出 GATT、讀寫 characteristic、把 **CCCD 設為 0x0002** 並接收 indication |
| `gattlib/build_gattlib.sh` | **Option 1**：在 RPi 上編譯並安裝 GATTLIB |
| `gattlib/my_write/my_write.c` | **Option 1**：用 C + GATTLIB 寫的 BLE central，負責掃描、配對、連線、寫入文字並讀回 |
| `option1_report.md` | **Option 1** 個人報告 |

環境：Raspberry Pi 3 Model B、Raspberry Pi OS（Debian 13 trixie）、BlueZ 5.82、Python 3.13、bluepy 1.3.0。

## iPhone 端設定（LightBlue）

1. LightBlue → Virtual Devices → **Blank**，畫面要停在前景，否則不會廣播名稱。
2. 目標 characteristic 是 service `0x1111` 底下的 `b3fcaed7-8598-419f-89f9-ebf46229fc7c`（READ / NOTIFY / INDICATE）。要寫入的話，記得在 LightBlue 打開它的 **Write** 屬性。

## Basic problem：設定 CCCD 為 0x0002

```bash
python3 -m venv ~/ble && ~/ble/bin/pip install bluepy
sudo ~/ble/bin/python3 ble_central.py --name Blank --service 1111 \
     --char b3fcaed7-8598-419f-89f9-ebf46229fc7c --cccd 0x0002 --wait 60
```

1. 掃描並找到名稱為 `Blank` 的裝置，連線後列出所有 service、characteristic 和 descriptor。
2. 把 `02 00`（0x0002，little-endian）寫入 CCCD，再讀回確認：

   ```
   Wrote CCCD = 0x0002
   CCCD read back = 0200
   ```

3. 接下來 60 秒，在 LightBlue 修改值，Pi 會收到 indication 並自動回 Confirmation。

可加上 `--write <hex>` 同時寫入值，例如 `--write 68656c6c6f2c204920616d2045737468657221`（"hello, I am Esther!"）。

錄封包當作證明：另開一個視窗執行 `sudo btmon`，就會看到以下封包：
- `ATT: Write Request (0x12)`：Handle `0x003e`（CCCD），Data `0200`
- `Handle Value Indication (0x1d)`
- `Handle Value Confirmation (0x1e)`

## Option 1：GATTLIB（C）

```bash
cd gattlib && bash build_gattlib.sh          # 編譯並安裝 GATTLIB（含 libpcre -> libpcre2-8 修正）
cd my_write && make
sudo ./my_write -c b3fcaed7-8598-419f-89f9-ebf46229fc7c -t "hello, I am Esther!"
```

出現 `Pairing: confirm passkey ...` 時，在 iPhone 上按「配對」。成功的話會顯示：

```
Wrote 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
Read back 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
```

設計、用到的 API、遇到的問題，以及和 bluepy 的比較，都寫在 [`option1_report.md`](option1_report.md)。

## 注意事項

- **先跑 Basic 再跑 Option 1。** `my_write` 會和 iPhone 配對；配對之後，bluepy 會看到 public identity address 而連不上。要再跑 Basic，請先執行 `bluetoothctl remove <位址>`，並在 iPhone 的「設定 → 藍牙」中忽略這台 Pi。
- iPhone 的 BLE 位址會一直變，所以用 `--name Blank` 掃描，不要寫死位址。
- RPi 電源不足（`vcgencmd get_throttled` 不是 `0x0`）時，Wi-Fi 和藍牙都會不穩，請使用 5V 2.5A 的電源。
