#!/usr/bin/env python3
# Pipe the output of GATTLIB's advertisement_data example through this script
# to append the Bluetooth SIG company name to every manufacturer id:
#
#   sudo stdbuf -oL ./advertisement_data/advertisement_data | python3 company_name.py
#
# Company list (official Bluetooth SIG assigned numbers), download once:
#   curl -fsSL -o ~/company_identifiers.yaml https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml

import os
import re
import sys

YAML = os.path.expanduser("~pi/company_identifiers.yaml")


def load_companies(path):
    # Tiny parser for the "- value: 0x004C / name: 'Apple, Inc.'" pairs,
    # so PyYAML is not needed.
    companies = {}
    value = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*- value: (0x[0-9A-Fa-f]+)", line)
            if m:
                value = int(m.group(1), 16)
                continue
            m = re.match(r"\s*name: '?(.*?)'?\s*$", line)
            if m and value is not None:
                companies[value] = m.group(1).replace("''", "'")
                value = None
    return companies


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else YAML
    companies = load_companies(path)

    def name_of(m):
        cid = int(m.group(1), 16)
        return "id 0x%04x (%s):" % (cid, companies.get(cid, "unknown / not assigned"))

    for line in sys.stdin:
        # the example prints no newline for devices without manufacturer data
        line = line.replace(": Device ", ":\nDevice ")
        sys.stdout.write(re.sub(r"id (0x[0-9a-fA-F]+):", name_of, line))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
