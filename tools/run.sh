#!/usr/bin/env bash
# Build + flash + monitor for one role on one port.
#
# Usage:
#   tools/run.sh [-r gateway|sensor] [-p PORT] [-n NODE_ID] [--fake|--real] [--erase]
#
# --erase wipes the NVS partition before flashing the new image.
# Useful when the mesh stack's NVS state (RPL, provisioning data) is
# stale and you want to start fresh. Bootloader + app + factory_nvs
# (OOB sticker) are preserved.
#
# Examples:
#   tools/run.sh -r gateway -p /dev/cu.usbmodem21
#   tools/run.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake
#   tools/run.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake --erase
#
# Exit the monitor with Ctrl-].

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf
require_port "$PORT"

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

OVERRIDE="$(write_kconfig_override "$ROLE" "$NODE_ID" "$FAKE_RADAR")"
trap 'rm -f "$OVERRIDE"' EXIT

echo "Run role=$ROLE port=$PORT dir=$BUILD_DIR sdkconfig=$SDKCONFIG erase=${ERASE:-n} (Ctrl-] to exit)"

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
    -p "$PORT" -b 460800 flash monitor
