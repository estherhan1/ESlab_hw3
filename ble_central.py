#!/usr/bin/env python3
# HW-Lab3: BLE Central (GATT client) on Raspberry Pi using bluepy.
#
# Steps:
#   1. scan for BLE peripherals (or connect directly with --addr)
#   2. connect and dump every service / characteristic / descriptor
#   3. read (and optionally write) the target characteristic
#   4. write the CCCD (UUID 0x2902) of the target characteristic
#      (default 0x0002 = enable indications), then read it back
#   5. wait for notifications / indications pushed by the phone
#
# Usage (on the RPi):
#   sudo python3 ble_central.py
#   sudo python3 ble_central.py --addr 4a:3b:xx:xx:xx:xx --cccd 0x0002
#   sudo python3 ble_central.py --name Blank --service 1111 --char <uuid>
#   sudo python3 ble_central.py --service fff0 --char fff1 --write 656667

import argparse
import struct
import sys

from bluepy.btle import (BTLEException, DefaultDelegate, Peripheral, Scanner,
                         UUID)

CCCD_UUID = UUID(0x2902)
PROP_WRITE = 0x08
PROP_NOTIFY = 0x10
PROP_INDICATE = 0x20


class ScanDelegate(DefaultDelegate):
    def __init__(self):
        DefaultDelegate.__init__(self)
        self.count = 0

    def handleDiscovery(self, dev, isNewDev, isNewData):
        # Crowded places have hundreds of advertisers; show progress only.
        if isNewDev:
            self.count += 1
            if self.count % 10 == 0:
                print("  ... %d devices discovered" % self.count)


class NotifyDelegate(DefaultDelegate):
    def __init__(self):
        DefaultDelegate.__init__(self)

    def handleNotification(self, cHandle, data):
        print("  <<< notification/indication from handle 0x%04x: %s  (%r)"
              % (cHandle, data.hex(), data))


def parse_uuid(s):
    s = s.lower().replace("0x", "")
    return UUID(int(s, 16)) if len(s) <= 8 else UUID(s)


def scan_and_pick(timeout, want_name=None):
    print("Scanning for %.1f s ..." % timeout)
    scanner = Scanner().withDelegate(ScanDelegate())
    devices = [d for d in scanner.scan(timeout) if d.connectable]
    if not devices:
        sys.exit("No BLE device found. Is the phone advertising?")

    def name_of(dev):
        return dev.getValueText(9) or dev.getValueText(8) or ""

    if want_name:
        found = [d for d in devices if name_of(d) == want_name]
        if not found:
            sys.exit("No device named '%s' found. Is the phone advertising?"
                     % want_name)
        dev = max(found, key=lambda d: d.rssi)
        print("Found '%s' at %s (%s), RSSI=%d dB"
              % (want_name, dev.addr, dev.addrType, dev.rssi))
        return dev.addr, dev.addrType

    # Named devices (the phone app advertises a name), plus the 5 strongest
    # unnamed ones; crowded places can have hundreds of anonymous advertisers.
    named = sorted([d for d in devices if name_of(d)], key=lambda d: -d.rssi)
    unnamed = sorted([d for d in devices if not name_of(d)], key=lambda d: -d.rssi)
    devices = named + unnamed[:5]
    print("\nConnectable devices (%d unnamed ones hidden):"
          % max(0, len(unnamed) - 5))
    for n, dev in enumerate(devices):
        print("%2d: %s (%s), RSSI=%d dB, name=%s"
              % (n, dev.addr, dev.addrType, dev.rssi, name_of(dev) or "(no name)"))
    num = int(input("Enter your device number: "))
    return devices[num].addr, devices[num].addrType


def dump_gatt(dev):
    # Discover everything in one pass over the whole handle range; walking
    # service by service makes some phones (iOS) drop the connection.
    services = list(dev.getServices())
    chars = dev.getCharacteristics()
    try:
        descs = dev.getDescriptors()
    except BTLEException:
        descs = []
    print("\n===== GATT database =====")
    for svc in services:
        print("Service %s  handles 0x%04x-0x%04x"
              % (svc.uuid, svc.hndStart, svc.hndEnd))
        for ch in chars:
            if not svc.hndStart <= ch.getHandle() <= svc.hndEnd:
                continue
            print("  Char %s  handle=0x%04x  props=%s"
                  % (ch.uuid, ch.getHandle(), ch.propertiesToString()))
            for d in descs:
                if d.uuid == CCCD_UUID and d.handle == ch.getHandle() + 1:
                    print("    Desc %s (CCCD)  handle=0x%04x" % (d.uuid, d.handle))
    print("=========================\n")


def find_target_char(dev, svc_uuid, char_uuid, want_prop, strict):
    try:
        svc = dev.getServiceByUUID(svc_uuid)
        chars = [ch for ch in dev.getCharacteristics(svc.hndStart, svc.hndEnd)
                 if ch.uuid == char_uuid]
        if chars:
            return chars[0]
    except BTLEException:
        pass
    if strict:
        # The user named this exact characteristic: never touch another one,
        # it is most likely the wrong device.
        sys.exit("Char %s in service %s not found -- wrong device?"
                 % (char_uuid, svc_uuid))
    # Fallback: last characteristic with the wanted property (app-defined
    # services come after the phone's built-in ones), else any notify/indicate.
    print("Char %s in service %s not found; picking a %s char instead"
          % (char_uuid, svc_uuid,
             "INDICATE" if want_prop == PROP_INDICATE else "NOTIFY"))
    chars = dev.getCharacteristics()
    for prop in (want_prop, PROP_NOTIFY | PROP_INDICATE):
        matches = [ch for ch in chars if ch.properties & prop]
        if matches:
            return matches[-1]
    sys.exit("No characteristic supporting notify/indicate was found.")


def find_cccd(ch):
    try:
        descs = ch.getDescriptors(forUUID=CCCD_UUID)
        if descs:
            return descs[0].handle
    except BTLEException:
        pass
    # The CCCD normally sits right after the characteristic value handle.
    handle = ch.getHandle() + 1
    print("CCCD not discovered, assuming handle 0x%04x" % handle)
    return handle


def main():
    ap = argparse.ArgumentParser(description="BLE central / GATT client demo")
    ap.add_argument("--addr", help="peripheral address (skip scanning)")
    ap.add_argument("--addr-type", default="random", choices=["random", "public"],
                    help="address type when --addr is given (phones use random)")
    ap.add_argument("--name", help="connect to the device advertising this name, e.g. Blank")
    ap.add_argument("--scan-time", type=float, default=10.0)
    ap.add_argument("--service", help="target service UUID (default fff0)")
    ap.add_argument("--char", help="target characteristic UUID (default fff1)")
    ap.add_argument("--write", help="hex bytes to write to the characteristic, e.g. 656667")
    ap.add_argument("--cccd", default="0x0002",
                    help="CCCD value: 0x0001 notify, 0x0002 indicate, 0x0000 off")
    ap.add_argument("--wait", type=float, default=30.0,
                    help="seconds to wait for notifications/indications")
    args = ap.parse_args()

    if args.addr:
        addr, addr_type = args.addr, args.addr_type
    else:
        addr, addr_type = scan_and_pick(args.scan_time, args.name)

    print("Connecting to %s (%s) ..." % (addr, addr_type))
    dev = Peripheral(addr, addr_type)
    dev.setDelegate(NotifyDelegate())
    try:
        dump_gatt(dev)

        cccd_value = int(args.cccd, 0)
        want_prop = PROP_INDICATE if cccd_value & 0x0002 else PROP_NOTIFY
        strict = bool(args.service or args.char)
        ch = find_target_char(dev, parse_uuid(args.service or "fff0"),
                              parse_uuid(args.char or "fff1"), want_prop, strict)
        print("Target char %s handle=0x%04x props=%s"
              % (ch.uuid, ch.getHandle(), ch.propertiesToString()))

        if ch.supportsRead():
            # iOS may refuse a read (e.g. ATT error 0xF2) while the app has
            # no value set; that must not stop the CCCD demo.
            try:
                val = ch.read()
                print("Read value: %s  (%r)" % (val.hex(), val))
            except BTLEException as e:
                print("Read failed (%s), continuing" % e)

        if args.write and not ch.properties & PROP_WRITE:
            print("Skipping write: characteristic has no WRITE property "
                  "(enable Write for it in the phone app)")
        elif args.write:
            data = bytes.fromhex(args.write)
            ch.write(data, withResponse=True)
            print("Wrote value: %s  (%r)" % (data.hex(), data))
            if ch.supportsRead():
                try:
                    val = ch.read()
                    print("Read back : %s  (%r)" % (val.hex(), val))
                except BTLEException as e:
                    print("Read back failed (%s)" % e)

        # ---- CCCD setting ----
        cccd_handle = find_cccd(ch)
        print("\nCCCD handle=0x%04x, current value=%s"
              % (cccd_handle, dev.readCharacteristic(cccd_handle).hex()))
        if cccd_value & 0x0002 and not ch.properties & PROP_INDICATE:
            print("Warning: characteristic does not advertise INDICATE")
        if cccd_value & 0x0001 and not ch.properties & PROP_NOTIFY:
            print("Warning: characteristic does not advertise NOTIFY")
        # CCCD is a 16-bit little-endian value: 0x0002 -> b'\x02\x00'
        dev.writeCharacteristic(cccd_handle, struct.pack("<H", cccd_value),
                                withResponse=True)
        print("Wrote CCCD = 0x%04x" % cccd_value)
        print("CCCD read back = %s" % dev.readCharacteristic(cccd_handle).hex())

        if cccd_value and args.wait > 0:
            print("\nWaiting %.0f s for notifications/indications "
                  "(press 'Notify'/'Indicate' in the phone app) ..." % args.wait)
            remaining = args.wait
            while remaining > 0:
                dev.waitForNotifications(1.0)
                remaining -= 1.0
    finally:
        dev.disconnect()
        print("Disconnected.")


if __name__ == "__main__":
    main()
