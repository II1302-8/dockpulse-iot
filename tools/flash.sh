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

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf
require_port "$PORT"

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

OVERRIDE="$(write_kconfig_override "$ROLE" "$NODE_ID" "$FAKE_RADAR")"
trap 'rm -f "$OVERRIDE"' EXIT

TARGET=${APP_ONLY:+app-flash}
TARGET=${TARGET:-flash}

echo "Flash role=$ROLE port=$PORT dir=$BUILD_DIR sdkconfig=$SDKCONFIG erase=${ERASE:-n} target=$TARGET"

sync_sdkconfig "$SDKCONFIG" "$ROLE" "$NODE_ID" "$FAKE_RADAR"

if [[ -n "$ERASE" ]]; then
    # wipe nvs only (mesh stack creds, dp_prov table), keep bootloader,
    # app, and factory_nvs so the OOB sticker still matches. parttool
    # reads partitions.csv so a future ptable change can't corrupt
    # other partitions silently
    parttool.py --port "$PORT" --baud 460800 \
        --partition-table-file partitions.csv \
        erase_partition --partition-name nvs
fi

exec idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERRIDE" \
    -p "$PORT" -b 460800 "$TARGET"
