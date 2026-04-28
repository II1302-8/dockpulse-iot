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

exec idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -p "$PORT" monitor
