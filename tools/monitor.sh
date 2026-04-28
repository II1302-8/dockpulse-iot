#!/usr/bin/env bash
# Open serial monitor on a port. No build dir needed — pure UART tap.
#
# Usage:
#   tools/monitor.sh [-p PORT] [-r gateway|sensor]
#
# --role only matters if you want symbol decoding from the elf in the
# corresponding build dir. Without --role, falls back to the default
# build/.
# Exit the monitor with Ctrl-].

print_help() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf
require_port "$PORT"

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

echo "Monitor role=$ROLE port=$PORT dir=$BUILD_DIR (Ctrl-] to exit)"

# --no-reset: opening the USB-Serial-JTAG port otherwise sends a chip
# reset signal (rst:0x15 USB_UART_CHIP_RESET on ESP32-C3), restarting
# the sensor's mesh seq counter and dropping every packet from before.
# We're observers here, not flashers — never reset on connect.
exec idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -p "$PORT" monitor --no-reset
