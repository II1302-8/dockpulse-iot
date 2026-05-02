#!/usr/bin/env bash
# Build + flash one role to a port.
#
# Usage:
#   tools/flash.sh [-r gateway|sensor] [-p PORT] [-n NODE_ID] [--fake|--real] [-a]
#
# Examples:
#   tools/flash.sh -r gateway -p /dev/cu.usbmodem21
#   tools/flash.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake
#   tools/flash.sh -r gateway -a              # app-flash, skip bootloader+ptable
#
# With a single board attached, port auto-detects to the first
# /dev/cu.usbmodem*. With multiple boards you must pass --port.
#
# -a / --app-only flashes only the app partition. Saves ~5s per cycle.
# Use after the first full flash for the role (bootloader+ptable already
# present on the device).

print_help() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf
require_port "$PORT"

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

OVERRIDE="$(mktemp -t dp_flash_XXXX.cfg)"
trap 'rm -f "$OVERRIDE"' EXIT
case "$ROLE" in
    gateway) echo "CONFIG_DOCKPULSE_ROLE_GATEWAY=y" >>"$OVERRIDE" ;;
    sensor)  echo "CONFIG_DOCKPULSE_ROLE_SENSOR=y"  >>"$OVERRIDE" ;;
esac
if [[ -n "$NODE_ID" ]]; then
    echo "CONFIG_DOCKPULSE_NODE_ID=$NODE_ID" >>"$OVERRIDE"
fi
if [[ "$ROLE" == "sensor" && -n "$FAKE_RADAR" ]]; then
    if [[ "$FAKE_RADAR" == y ]]; then
        echo "CONFIG_DOCKPULSE_RADAR_FAKE=y" >>"$OVERRIDE"
    else
        echo "# CONFIG_DOCKPULSE_RADAR_FAKE is not set" >>"$OVERRIDE"
    fi
fi

TARGET=${APP_ONLY:+app-flash}
TARGET=${TARGET:-flash}

echo "Flash role=$ROLE port=$PORT dir=$BUILD_DIR sdkconfig=$SDKCONFIG erase=${ERASE:-n} target=$TARGET"

sync_sdkconfig "$SDKCONFIG" "$ROLE" "$NODE_ID" "$FAKE_RADAR"

if [[ -n "$ERASE" ]]; then
    # ESP32-C3 ROM doesn't expose `erase_flash` over USB-Serial-JTAG
    # (idf.py auto-disables the flasher stub on USB). Erase just the
    # NVS partition via esptool's ROM-supported `erase_region`.
    # Offset/size from partitions.csv: nvs @ 0x9000, 0x6000 bytes.
    python -m esptool --chip esp32c3 --port "$PORT" --baud 460800 --no-stub \
        erase_region 0x9000 0x6000
fi

exec idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERRIDE" \
    -p "$PORT" -b 460800 "$TARGET"
