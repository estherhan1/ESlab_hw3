#!/bin/bash
# Optional problem 1: build GATTLIB (C BLE library on top of BlueZ) on the RPi
# and its examples. Run on the Raspberry Pi:  bash build_gattlib.sh
set -e

sudo apt update
sudo apt install -y git cmake build-essential pkg-config \
    libbluetooth-dev libglib2.0-dev libreadline-dev libpcre3-dev

if [ ! -d gattlib-src ]; then
    git clone https://github.com/labapart/gattlib.git gattlib-src
fi
cd gattlib-src
mkdir -p build && cd build
cmake .. -DGATTLIB_PYTHON_INTERFACE=OFF
make -j"$(nproc)"

echo
echo "Built examples:"
find examples -maxdepth 2 -type f -executable
