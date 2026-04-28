# Shared helpers for tools/*. Source this file; do not execute it.
#
# Activates ESP-IDF, parses --role/--port/--node-id flags, and resolves
# the build directory and a port to use.

set -euo pipefail

# Reconcile the existing sdkconfig in BUILD_DIR with the requested
# role/node_id/fake-radar flags. SDKCONFIG_DEFAULTS only takes effect
# on first config — so once a build dir exists we have to mutate
# sdkconfig directly. We do that by rewriting (or appending) the
# specific Kconfig lines, which CMake will then pick up on the next
# `idf.py build`.
sync_sdkconfig() {
    local sdkconfig="$1" role="$2" node_id="$3" fake="$4"
    [[ -f "$sdkconfig" ]] || return 0

    # Role: clear both and set the wanted one.
    _kconfig_unset "$sdkconfig" CONFIG_DOCKPULSE_ROLE_SENSOR
    _kconfig_unset "$sdkconfig" CONFIG_DOCKPULSE_ROLE_GATEWAY
    case "$role" in
        gateway) _kconfig_set "$sdkconfig" CONFIG_DOCKPULSE_ROLE_GATEWAY y ;;
        sensor)  _kconfig_set "$sdkconfig" CONFIG_DOCKPULSE_ROLE_SENSOR y ;;
    esac

    if [[ -n "$node_id" ]]; then
        _kconfig_set "$sdkconfig" CONFIG_DOCKPULSE_NODE_ID "$node_id"
    fi

    if [[ "$role" == "sensor" && -n "$fake" ]]; then
        if [[ "$fake" == y ]]; then
            _kconfig_set "$sdkconfig" CONFIG_DOCKPULSE_RADAR_FAKE y
        else
            _kconfig_unset "$sdkconfig" CONFIG_DOCKPULSE_RADAR_FAKE
        fi
    fi
}

# Set CONFIG_FOO=value, replacing existing line or appending. Handles
# both `KEY=...` and `# KEY is not set` forms. Uses two simple seds
# rather than one nested-alternation pattern, because BSD sed (the
# default on macOS) trips on nested groups even with -E.
_kconfig_set() {
    local file="$1" key="$2" value="$3"
    if grep -qE "^${key}=" "$file"; then
        sed -i.bak -e "s|^${key}=.*|${key}=${value}|" "$file"
        rm -f "$file.bak"
    elif grep -qE "^# ${key} is not set" "$file"; then
        sed -i.bak -e "s|^# ${key} is not set|${key}=${value}|" "$file"
        rm -f "$file.bak"
    else
        echo "${key}=${value}" >>"$file"
    fi
}

# Set CONFIG_FOO to "is not set" (Kconfig's idiom for unset bool).
_kconfig_unset() {
    local file="$1" key="$2"
    if grep -qE "^${key}=" "$file"; then
        sed -i.bak -e "s|^${key}=.*|# ${key} is not set|" "$file"
        rm -f "$file.bak"
    fi
}

activate_idf() {
    if command -v idf.py >/dev/null 2>&1; then
        return
    fi
    : "${IDF_PATH:=$HOME/esp/esp-idf}"
    if [[ ! -f "$IDF_PATH/export.sh" ]]; then
        echo "ESP-IDF not found at $IDF_PATH. Set IDF_PATH or install per CONTRIBUTING.md." >&2
        exit 1
    fi
    # shellcheck source=/dev/null
    . "$IDF_PATH/export.sh" >/dev/null
}

# Build dir convention: gateway → build/, sensor → build_sensor/.
# Two persistent build dirs avoid menuconfig thrash when swapping roles.
build_dir_for() {
    case "$1" in
        gateway) echo "build" ;;
        sensor)  echo "build_sensor" ;;
        *) echo "Unknown role: $1 (expected: gateway, sensor)" >&2; exit 2 ;;
    esac
}

# sdkconfig lives at the project root by default in IDF — meaning two
# build dirs share one sdkconfig and clobber each other on role swap.
# Give each role its own file via -DSDKCONFIG=<this>.
sdkconfig_for() {
    case "$1" in
        gateway) echo "sdkconfig" ;;       # keep the canonical name for the
                                            # default role so manual `idf.py
                                            # menuconfig` still works without
                                            # remembering to pass -DSDKCONFIG
        sensor)  echo "sdkconfig.sensor" ;;
        *) echo "Unknown role: $1" >&2; exit 2 ;;
    esac
}

# First /dev/cu.usbmodem* device, or empty if none. Note: with multiple
# boards plugged in, this is non-deterministic — pass --port explicitly.
default_port() {
    ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true
}

require_port() {
    if [[ -z "${1:-}" ]]; then
        echo "No /dev/cu.usbmodem* device found. Plug in a board or pass --port." >&2
        echo "Hint: tools/list-ports.sh shows attached boards." >&2
        exit 1
    fi
}

# Parse --role / --port / --node-id / --fake / --real flags. Sets:
#   ROLE     gateway|sensor (default: gateway)
#   PORT     /dev/cu.usbmodem* (default: first detected)
#   NODE_ID  integer 1..255 (optional; empty = use whatever's in the
#                            existing sdkconfig)
#   FAKE_RADAR  empty | y | n (sensor-only)
# Remaining unparsed args are left in REST=("${@}").
parse_args() {
    ROLE=gateway
    PORT=""
    NODE_ID=""
    FAKE_RADAR=""
    ERASE=""
    REST=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -r|--role)      ROLE="$2"; shift 2 ;;
            -p|--port)      PORT="$2"; shift 2 ;;
            -n|--node-id)   NODE_ID="$2"; shift 2 ;;
            --fake)         FAKE_RADAR=y; shift ;;
            --real)         FAKE_RADAR=n; shift ;;
            --erase)        ERASE=y; shift ;;
            -h|--help)      print_help; exit 0 ;;
            --)             shift; REST+=("$@"); break ;;
            *)              REST+=("$1"); shift ;;
        esac
    done
    if [[ -z "$PORT" ]]; then
        PORT="$(default_port)"
    fi
}
