#!/usr/bin/env bash
# Generate the Ed25519 factory keypair used to sign claim JWTs.
#
# Run once per provisioning host; the priv key never leaves it. The pub
# key gets pasted into the backend's FACTORY_PUBKEY env var (see
# docker-compose.{staging,prod}.yml in the dockpulse repo).
#
# Usage:
#   tools/factory-keygen.sh                # creates factory/private/factory.{pem,pub.pem}
#   tools/factory-keygen.sh --force        # overwrite existing keypair
#
# After running, copy factory.pub.pem contents into the backend's
# FACTORY_PUBKEY env. Devices flashed before a key rotation keep working
# only as long as the old pub key is still trusted by the backend, so
# rotate carefully.

set -euo pipefail

cd "$(dirname "$0")/.."
PRIV="factory/private/factory.pem"
PUB="factory/private/factory.pub.pem"

FORCE=""
[[ "${1:-}" == "--force" ]] && FORCE=y

if [[ -e "$PRIV" && -z "$FORCE" ]]; then
    echo "$PRIV already exists. Pass --force to overwrite." >&2
    exit 1
fi

mkdir -p "$(dirname "$PRIV")"

# belt-and-braces if a future contributor moves factory/private/ out from
# under .gitignore, ask git directly
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
    if ! git check-ignore -q "$PRIV"; then
        echo "refusing to write $PRIV: not in .gitignore (would be committed)." >&2
        echo "add factory/private/ to .gitignore first." >&2
        exit 1
    fi
fi

openssl genpkey -algorithm Ed25519 -out "$PRIV"
chmod 600 "$PRIV"
openssl pkey -in "$PRIV" -pubout -out "$PUB"

echo "wrote $PRIV (mode 600)"
echo "wrote $PUB"
echo
echo "FACTORY_PUBKEY env value (paste into backend deployment):"
echo "---8<---"
cat "$PUB"
echo "---8<---"
