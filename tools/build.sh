#!/usr/bin/env bash
# Build firmware for one role into the role-specific build dir.
#
# Usage:
#   tools/build.sh [-r gateway|sensor] [-n NODE_ID] [--fake|--real] [-a]
#
# Examples:
#   tools/build.sh -r gateway -n 1
#   tools/build.sh -r sensor  -n 2 --fake     # bench-test, no radar wired
#   tools/build.sh -r sensor  -n 3 --real     # real HMMD attached
#   tools/build.sh -r gateway -a              # app only, skip bootloader+ptable
#
# The first build for a role generates a fresh sdkconfig from
# sdkconfig.defaults plus a temporary one-shot override; subsequent
# builds reuse that sdkconfig (so menuconfig changes are sticky).
#
# -a / --app-only builds only the app target. Skips bootloader and
# partition table regen, saves ~3s on incremental builds. Use after
# the first full build for the role.

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

OVERRIDE="$(write_kconfig_override "$ROLE" "$NODE_ID" "$FAKE_RADAR")"
trap 'rm -f "$OVERRIDE"' EXIT

TARGET=${APP_ONLY:+app}
TARGET=${TARGET:-build}

echo "Build role=$ROLE dir=$BUILD_DIR sdkconfig=$SDKCONFIG node_id=${NODE_ID:-<sticky>} fake=${FAKE_RADAR:-<sticky>} target=$TARGET"

sync_sdkconfig "$SDKCONFIG" "$ROLE" "$NODE_ID" "$FAKE_RADAR"

exec idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERRIDE" "$TARGET"
