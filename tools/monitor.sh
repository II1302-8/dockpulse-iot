#!/usr/bin/env bash
# Open serial monitor on a port. No build dir needed — pure UART tap.
#
# Usage:
#   tools/monitor.sh -r gateway|sensor [-p PORT]
#
# -r/--role is required so symbol decoding picks the matching elf. Picking
# the wrong elf turns panic backtraces into nonsense.
# Exit the monitor with Ctrl-].

cd "$(dirname "$0")/.."
. tools/_common.sh

# detect explicit -r/--role before parse_args sets the default
USER_PROVIDED_ROLE=""
for arg in "$@"; do
    case "$arg" in -r|--role) USER_PROVIDED_ROLE=y ;; esac
done

parse_args "$@"
if [[ -z "$USER_PROVIDED_ROLE" ]]; then
    echo "monitor.sh requires -r gateway|sensor (so symbol decoding uses the right elf)." >&2
    exit 1
fi
activate_idf
require_port "$PORT"

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "$BUILD_DIR does not exist. Run tools/build.sh -r $ROLE first." >&2
    exit 1
fi

echo "Monitor role=$ROLE port=$PORT dir=$BUILD_DIR (Ctrl-] to exit)"

# --no-reset: opening the USB-Serial-JTAG port otherwise sends a chip
# reset signal (rst:0x15 USB_UART_CHIP_RESET on ESP32-C3), restarting
# the sensor's mesh seq counter and dropping every packet from before.
# We're observers here, not flashers — never reset on connect.
exec idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -p "$PORT" monitor --no-reset
