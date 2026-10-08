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
iPhone 熱點下 Pi 的 IP 是 `172.20.10.2`；bluepy 裝在 venv `~/ble`。
LightBlue 的 Blank 虛擬裝置廣播名稱為 `Blank`，其 service `0x1111` 內的
`b3fcaed7-8598-419f-89f9-ebf46229fc7d`（READ + INDICATE，CCCD handle 0x003e）就是目標：

```bash
ssh pi@172.20.10.2
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

```bash
cd gattlib && bash build_gattlib.sh     # patches libpcre -> libpcre2-8 for Debian 13
cd gattlib-src/build
sudo ./examples/ble_scan/ble_scan                       # scan
sudo ./examples/discover/discover XX:XX:XX:XX:XX:XX     # list services/chars
sudo ./examples/read_write/read_write XX:XX:XX:XX:XX:XX read  0000fff1-0000-1000-8000-00805f9b34fb
sudo ./examples/read_write/read_write XX:XX:XX:XX:XX:XX write 0000fff1-0000-1000-8000-00805f9b34fb 0x65
sudo ./examples/notification/notification XX:XX:XX:XX:XX:XX   # check its source for the UUID it subscribes to
```

Example arguments differ slightly between GATTLIB versions. Running an example with no arguments prints its usage.

### Report template (personal report, +5)
1. **Environment:** RPi 3, OS version, BlueZ version (`bluetoothctl --version`), GATTLIB commit.
2. **Build:** dependencies installed, any build errors and how you fixed them.
3. **Examples tried:** for each of ble_scan / discover / read_write / notification, give the command, a screenshot of the output, and a short explanation.
4. **Comparison with bluepy:** GATTLIB uses BlueZ over D-Bus and is asynchronous in recent versions, while bluepy drives `bluepy-helper` over HCI. Compare ease of use and the API differences.
5. **Problems encountered.**
