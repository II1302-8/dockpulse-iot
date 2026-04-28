#!/usr/bin/env bash
# List attached USB-serial devices likely to be ESP32 boards, with
# whatever identifying info is available (system_profiler on macOS,
# udevadm on Linux). Useful when you have multiple boards plugged in
# and need to figure out which port is which before passing --port.
#
# Usage: tools/list-ports.sh

set -euo pipefail

ports=()
for p in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$p" ]] && ports+=("$p")
done

if [[ ${#ports[@]} -eq 0 ]]; then
    echo "No USB-serial ports found."
    exit 0
fi

uname_s="$(uname -s)"

for port in "${ports[@]}"; do
    case "$uname_s" in
        Darwin)
            # Match port suffix back to system_profiler's USB tree.
            suffix="${port##*usbmodem}"
            line="$(system_profiler SPUSBDataType 2>/dev/null \
                    | awk -v s="$suffix" '
                        /Product ID|Vendor ID|Manufacturer|Product|Serial Number/ {
                            gsub(/^[ \t]+/, "");
                            buf = buf $0 "; ";
                        }
                        /Location ID/ { buf = ""; }
                        /Serial Number/ { if (buf ~ s) { print buf; exit } }
                    ')"
            ;;
        Linux)
            line="$(udevadm info --query=property --name="$port" 2>/dev/null \
                    | awk -F= '/^ID_VENDOR=|^ID_MODEL=|^ID_SERIAL_SHORT=/ {printf "%s=%s; ", $1, $2}')"
            ;;
        *)
            line=""
            ;;
    esac
    printf "%-30s %s\n" "$port" "${line:-<no metadata>}"
done
