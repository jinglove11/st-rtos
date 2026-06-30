#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK_VERSION=${PICO_SDK_VERSION:-2.2.0}
SDK_DIR="$ROOT/tools/pico-sdk"
PICOTOOL_SRC="$ROOT/tools/picotool-src"
PICOTOOL_BUILD="$ROOT/tools/picotool-build"
PICOTOOL_INSTALL="$ROOT/tools/picotool"
TMP_DIR=${TMPDIR:-/tmp}/myrtos-pico-setup

mkdir -p "$TMP_DIR" "$ROOT/tools"

if [[ ! -f "$SDK_DIR/pico_sdk_init.cmake" ]]; then
    echo "Downloading pico-sdk $SDK_VERSION"
    curl -L --fail --retry 3 \
        -o "$TMP_DIR/pico-sdk.tar.gz" \
        "https://github.com/raspberrypi/pico-sdk/archive/refs/tags/$SDK_VERSION.tar.gz"
    mkdir -p "$SDK_DIR"
    tar -xzf "$TMP_DIR/pico-sdk.tar.gz" -C "$SDK_DIR" --strip-components=1
fi

if [[ ! -x "$PICOTOOL_INSTALL/picotool/picotool" ]]; then
    echo "Building picotool $SDK_VERSION (UF2 tooling, no host USB dependency)"
    curl -L --fail --retry 3 \
        -o "$TMP_DIR/picotool.tar.gz" \
        "https://github.com/raspberrypi/picotool/archive/refs/tags/$SDK_VERSION.tar.gz"
    mkdir -p "$PICOTOOL_SRC"
    tar -xzf "$TMP_DIR/picotool.tar.gz" -C "$PICOTOOL_SRC" --strip-components=1
    cmake -S "$PICOTOOL_SRC" -B "$PICOTOOL_BUILD" \
        -DPICO_SDK_PATH="$SDK_DIR" \
        -DPICOTOOL_NO_LIBUSB=1 \
        -DPICOTOOL_FLAT_INSTALL=1 \
        -DCMAKE_INSTALL_PREFIX="$PICOTOOL_INSTALL"
    cmake --build "$PICOTOOL_BUILD" -j"${JOBS:-4}"
    cmake --install "$PICOTOOL_BUILD"
fi

echo "Pico SDK ready: $SDK_DIR"
echo "picotool ready: $PICOTOOL_INSTALL/picotool/picotool"

