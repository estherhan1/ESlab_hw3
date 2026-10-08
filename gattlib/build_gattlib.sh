#!/bin/bash
# Optional problem 1: build GATTLIB (C BLE library on top of BlueZ) on the RPi
# and its examples. Run on the Raspberry Pi:  bash build_gattlib.sh
set -e

sudo apt update
sudo apt install -y git cmake build-essential pkg-config \
    libbluetooth-dev libglib2.0-dev libreadline-dev libpcre2-dev

if [ ! -d gattlib-src ]; then
    git clone --depth 1 https://github.com/labapart/gattlib.git gattlib-src
fi
cd gattlib-src

# The examples link against PCRE1 (libpcre), which Debian 13 / Raspberry Pi OS
# trixie no longer ships. No example code actually uses it, so link PCRE2.
grep -rl "REQUIRED libpcre)" examples | xargs -r sed -i "s/REQUIRED libpcre)/REQUIRED libpcre2-8)/"

mkdir -p build && cd build
# printf logging so errors show up in the terminal instead of syslog
cmake .. -DGATTLIB_LOG_BACKEND=printf -DGATTLIB_LOG_LEVEL=1
make -j2

echo
echo "Built examples:"
find examples -maxdepth 2 -type f -executable
