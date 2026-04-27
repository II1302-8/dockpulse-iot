#!/usr/bin/env bash
# Build + flash. Auto-detects ESP32-C3 on /dev/cu.usbmodem*.
# Usage: tools/flash.sh [PORT]
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v idf.py >/dev/null 2>&1; then
    : "${IDF_PATH:=$HOME/esp/esp-idf}"
    if [[ ! -f "$IDF_PATH/export.sh" ]]; then
        echo "ESP-IDF not found at $IDF_PATH. Set IDF_PATH or install per CONTRIBUTING.md." >&2
        exit 1
    fi
    # shellcheck source=/dev/null
    . "$IDF_PATH/export.sh" >/dev/null
fi

if [[ $# -ge 1 ]]; then
    PORT="$1"
else
    PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)"
    if [[ -z "$PORT" ]]; then
        echo "No /dev/cu.usbmodem* device found. Plug in the ESP32-C3 or pass a port." >&2
        exit 1
    fi
fi

echo "Flashing $PORT"
exec idf.py -p "$PORT" -b 460800 flash
