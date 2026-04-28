#!/usr/bin/env bash
# Build + flash one role to a port.
#
# Usage:
#   tools/flash.sh [-r gateway|sensor] [-p PORT] [-n NODE_ID] [--fake|--real]
#
# Examples:
#   tools/flash.sh -r gateway -p /dev/cu.usbmodem21
#   tools/flash.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake
#
# With a single board attached, port auto-detects to the first
# /dev/cu.usbmodem*. With multiple boards you must pass --port.

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

echo "Flash role=$ROLE port=$PORT dir=$BUILD_DIR sdkconfig=$SDKCONFIG erase=${ERASE:-n}"

sync_sdkconfig "$SDKCONFIG" "$ROLE" "$NODE_ID" "$FAKE_RADAR"

if [[ -n "$ERASE" ]]; then
    idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -p "$PORT" -b 460800 erase-flash
fi

exec idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERRIDE" \
    -p "$PORT" -b 460800 flash
