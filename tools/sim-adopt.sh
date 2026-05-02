#!/usr/bin/env bash
# bench helper for QR adoption. composes mesh UUID from MAC pushes a
# fake provision/req to local mosquitto waits for resp
#
# usage:
#   tools/sim-adopt.sh -m AABBCCDDEEFF -b ksss-saltsjobaden-pier-1-t1
#   tools/sim-adopt.sh -u <full-32-hex-uuid> -g gw-001 -h 127.0.0.1
#
# MAC or full UUID comes from sensor boot log line:
#   I (1234) dp_mesh: ready role=sensor uuid=aabbccddeeff444f434b50554c534501
# first 12 hex = MAC rest = DOCKPULSE marker firmware appends

set -euo pipefail

print_help() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

MAC=""
BERTH=""
GW_ID="gw-001"
HOST="127.0.0.1"
PORT="1883"
REQ_ID="bench-$(date +%s)"
TTL_S=60
# placeholder OOB from dp_mesh.c must match gateway set_static_oob_value
OOB_HEX="64702d7374617469632d6f6f62000000"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -m|--mac|-u|--uuid) MAC="$2"; shift 2 ;;
        -b|--berth)         BERTH="$2"; shift 2 ;;
        -g|--gateway-id)    GW_ID="$2"; shift 2 ;;
        -h|--host)          HOST="$2"; shift 2 ;;
        --port)             PORT="$2"; shift 2 ;;
        --req-id)           REQ_ID="$2"; shift 2 ;;
        --ttl)              TTL_S="$2"; shift 2 ;;
        --oob)              OOB_HEX="$2"; shift 2 ;;
        --help)             print_help; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$MAC" ]] || { echo "missing -m MAC (12 hex) or -u UUID (32 hex)" >&2; exit 2; }
[[ -n "$BERTH" ]] || { echo "missing -b BERTH_ID" >&2; exit 2; }

# strip colons dashes spaces lowercase
HEX=$(printf '%s' "$MAC" | tr -d ':-' | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')
MARKER_HEX="444f434b50554c5345"  # DOCKPULSE
case ${#HEX} in
    12) UUID_HEX="${HEX}${MARKER_HEX}01" ;;  # bare MAC append marker
    32) UUID_HEX="$HEX" ;;                    # full UUID use as-is
    *)  echo "expected 12 hex (MAC) or 32 hex (UUID) got ${#HEX}: '$HEX'" >&2; exit 2 ;;
esac

REQ_TOPIC="dockpulse/v1/gw/${GW_ID}/provision/req"
RESP_TOPIC="dockpulse/v1/gw/${GW_ID}/provision/resp"

PAYLOAD=$(cat <<EOF
{"req_id":"${REQ_ID}","uuid":"${UUID_HEX}","oob":"${OOB_HEX}","ttl_s":${TTL_S},"berth_id":"${BERTH}"}
EOF
)

echo "publish to ${HOST}:${PORT} ${REQ_TOPIC}"
echo "  payload: ${PAYLOAD}"
mosquitto_pub -h "$HOST" -p "$PORT" -t "$REQ_TOPIC" -m "$PAYLOAD" -q 1

echo
echo "waiting on ${RESP_TOPIC} (timeout $((TTL_S + 5))s)..."
# -C 1 exit after first match -W timeout wrapper
mosquitto_sub -h "$HOST" -p "$PORT" -t "$RESP_TOPIC" -W $((TTL_S + 5)) -C 1
