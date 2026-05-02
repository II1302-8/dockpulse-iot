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

print_help() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

cd "$(dirname "$0")/.."
. tools/_common.sh

parse_args "$@"
activate_idf

BUILD_DIR="$(build_dir_for "$ROLE")"
SDKCONFIG="$(sdkconfig_for "$ROLE")"

# Build a one-shot Kconfig override capturing the runtime knobs the
# user passed in. This file is consumed by SDKCONFIG_DEFAULTS only the
# first time the build dir is created — once sdkconfig exists it sticks.
OVERRIDE="$(mktemp -t dp_build_XXXX.cfg)"
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

TARGET=${APP_ONLY:+app}
TARGET=${TARGET:-build}

echo "Build role=$ROLE dir=$BUILD_DIR sdkconfig=$SDKCONFIG node_id=${NODE_ID:-<sticky>} fake=${FAKE_RADAR:-<sticky>} target=$TARGET"

sync_sdkconfig "$SDKCONFIG" "$ROLE" "$NODE_ID" "$FAKE_RADAR"

exec idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$SDKCONFIG" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERRIDE" "$TARGET"
