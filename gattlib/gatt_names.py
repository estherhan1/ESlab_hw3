#!/usr/bin/env python3
# Pipe the output of GATTLIB's ble_scan / discover examples through this script
# to add readable names to service / characteristic UUIDs and properties:
#
#   sudo stdbuf -oL ./ble_scan/ble_scan | python3 gatt_names.py
#
# UUID lists (official Bluetooth SIG assigned numbers), download once into ~pi:
#   B=https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/uuids
#   for f in service_uuids characteristic_uuids member_uuids; do
#       curl -fsSL -o ~/$f.yaml $B/$f.yaml; done

import os
import re
import sys

YAML_DIR = os.path.expanduser("~pi")
SIG_BASE = "-0000-1000-8000-00805f9b34fb"

# Vendor (128-bit) UUIDs published by Apple; not in the SIG lists.
VENDOR = {
    "7905f431-b5ce-4e99-a40f-4b1e122d00d0": "Apple Notification Center Service (ANCS)",
    "9fbf120d-6301-42d9-8c58-25e699a21dbd": "ANCS Notification Source",
    "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9": "ANCS Control Point",
    "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb": "ANCS Data Source",
    "89d3502b-0f36-433a-8ef4-c502ad55f8dc": "Apple Media Service (AMS)",
    "9b3c81d8-57b1-4a8a-b8df-0e56f7ca51c2": "AMS Remote Command",
    "2f7cabce-808d-411f-9a0c-bb92ba96c102": "AMS Entity Update",
    "c6b2f38c-23ab-46d8-a6ab-a3a870bbd5d7": "AMS Entity Attribute",
    "d0611e78-bbb4-4591-a5f8-487910ae4366": "Apple Continuity Service",
    "8667556c-9a37-4c91-84ed-54ee27d90049": "Apple Continuity",
    "9fa480e0-4967-4542-9390-d343dc5d04ae": "Apple Nearby Service",
    "af0badb1-5b99-43cd-917a-a77bc549e3cc": "Apple Nearby",
}

# GATT characteristic property bits (Core spec Vol 3 Part G 3.3.1.1)
PROPS = [(0x01, "BROADCAST"), (0x02, "READ"), (0x04, "WRITE_NO_RSP"),
         (0x08, "WRITE"), (0x10, "NOTIFY"), (0x20, "INDICATE"),
         (0x40, "SIGNED_WRITE"), (0x80, "EXTENDED")]


def load_uuids(path, table):
    # Tiny parser for "- uuid: 0x180F / name: Battery" pairs (no PyYAML).
    uuid = None
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\s*- uuid: 0x([0-9A-Fa-f]+)", line)
                if m:
                    uuid = int(m.group(1), 16)
                    continue
                m = re.match(r"\s*name: '?(.*?)'?\s*$", line)
                if m and uuid is not None:
                    table.setdefault(uuid, m.group(1))
                    uuid = None
    except OSError:
        print("(could not read %s)" % path, file=sys.stderr)


def main():
    names = {}
    for f in ("service_uuids", "characteristic_uuids", "member_uuids"):
        load_uuids(os.path.join(YAML_DIR, f + ".yaml"), names)

    def uuid_name(text):
        text = text.lower()
        if text in VENDOR:
            return VENDOR[text]
        if text.startswith("0x"):
            short = int(text, 16)
        elif text.endswith(SIG_BASE):
            short = int(text[:8], 16)
        else:
            return "vendor specific"
        return names.get(short, "unknown")

    def props_name(value):
        v = int(value, 16)
        return "|".join(n for bit, n in PROPS if v & bit) or "none"

    for line in sys.stdin:
        line = line.rstrip("\n")
        m = re.search(r"uuid:(\S+)", line)
        if m:
            line += "  <- " + uuid_name(m.group(1))
        m = re.search(r"properties:([0-9a-fA-F]+)", line)
        if m:
            line += " [" + props_name(m.group(1)) + "]"
        print(line, flush=True)


if __name__ == "__main__":
    main()
