# HW-Lab3: BLE Central (GATT client)

A Raspberry Pi 3 acts as the BLE central. The peripheral is the LightBlue virtual device **Blank** on an iPhone.

| File | Purpose |
|---|---|
| `ble_central.py` | **Basic problem**: Python (bluepy) BLE central that scans, connects, dumps the GATT table, reads and writes a characteristic, **sets the CCCD to 0x0002**, and receives indications |
| `gattlib/build_gattlib.sh` | **Option 1**: builds and installs GATTLIB on the RPi |
| `gattlib/my_write/my_write.c` | **Option 1**: C + GATTLIB BLE central that scans, pairs, connects, writes a text string and reads it back |
| `option1_report.md` | **Option 1** personal report |

Environment: Raspberry Pi 3 Model B, Raspberry Pi OS (Debian 13 trixie), BlueZ 5.82, Python 3.13, bluepy 1.3.0.

## iPhone setup (LightBlue)

1. Open LightBlue → Virtual Devices → **Blank** and keep it in the foreground. In the background, iOS stops advertising the name.
2. The target is characteristic `b3fcaed7-8598-419f-89f9-ebf46229fc7c` (READ / NOTIFY / INDICATE) in service `0x1111`. To write to it, enable its **Write** property in LightBlue first.

## Basic problem: set the CCCD to 0x0002

```bash
python3 -m venv ~/ble && ~/ble/bin/pip install bluepy
sudo ~/ble/bin/python3 ble_central.py --name Blank --service 1111 \
     --char b3fcaed7-8598-419f-89f9-ebf46229fc7c --cccd 0x0002 --wait 60
```

1. The program scans for the device named `Blank`, connects, and lists every service, characteristic and descriptor.
2. It writes `02 00` (0x0002, little-endian) to the CCCD and reads it back:

   ```
   Wrote CCCD = 0x0002
   CCCD read back = 0200
   ```

3. For the next 60 seconds, change the value in LightBlue. The Pi receives each indication and sends the confirmation automatically.

Add `--write <hex>` to also write a value, for example `--write 68656c6c6f2c204920616d2045737468657221` ("hello, I am Esther!").

To capture proof, run `sudo btmon` in a second terminal. It shows these packets:
- `ATT: Write Request (0x12)` to handle `0x003e` (CCCD) with data `0200`
- `Handle Value Indication (0x1d)`
- `Handle Value Confirmation (0x1e)`

## Option 1: GATTLIB (C)

```bash
cd gattlib && bash build_gattlib.sh          # build + install GATTLIB (includes the libpcre -> libpcre2-8 fix)
cd my_write && make
sudo ./my_write -c b3fcaed7-8598-419f-89f9-ebf46229fc7c -t "hello, I am Esther!"
```

When `Pairing: confirm passkey ...` appears, tap **Pair** on the iPhone. On success the program prints:

```
Wrote 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
Read back 19 bytes: 68656c6c6f2c204920616d2045737468657221  "hello, I am Esther!"
```

[`option1_report.md`](option1_report.md) covers the design, the APIs used, the problems found, and a comparison with bluepy.

## Notes

- **Run the Basic problem before Option 1.** `my_write` pairs with the iPhone. After that, bluepy sees the phone's public identity address and fails to connect. To run the Basic problem again, run `bluetoothctl remove <address>` and choose "Forget This Device" for the Pi in the iPhone's Bluetooth settings.
- The iPhone's BLE address keeps changing, so find the phone with `--name Blank` and don't hard-code an address.
- Under-voltage makes Wi-Fi and Bluetooth unstable. If `vcgencmd get_throttled` does not print `0x0`, use a 5V 2.5A power supply.
