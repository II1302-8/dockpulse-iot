# Factory-flashing a new sensor node

Each board needs an app image, a per-device OOB blob in `factory_nvs`, and a
QR sticker. The backend keeps a `factory_devices` row pairing the serial to
its mesh UUID, OOB, and jti — the QR is just plaintext `serial:jti`
(~45 chars, ~37×37 modules). The real auth happens at PB-ADV provisioning
time via the OOB inside `factory_nvs`, not via the sticker itself, so it can
stay small.

`tools/factory-flash.py` handles all of it in one shot, including the backend
PUT.

## Host setup (once per workstation)

```bash
brew install qrencode          # needed for the sticker PNG
```

No factory key to manage anymore. The QR is unsigned plaintext; the OOB-based
PB-ADV handshake is the real authentication boundary.

### Backend credentials

`factory-flash.py` registers each board against the backend admin endpoint
during the flash. Provision a Cloudflare Access service token once and export
three env vars on the operator's shell (`~/.zshrc` or a `direnv` `.envrc` next
to this repo — do **not** put them in a committed `.env`):

```bash
export DP_BACKEND_URL=https://admin.dockpulse.xyz
export DP_CF_ACCESS_CLIENT_ID=<service-token-id>
export DP_CF_ACCESS_CLIENT_SECRET=<service-token-secret>
```

How to create the service token:

1. Cloudflare Zero Trust → Access → Service Auth → Service Tokens →
   **Create Service Token**, name it `dockpulse-factory-flash`. Copy both
   Client ID and Client Secret (the secret is only shown once).
2. Zero Trust → Access → Applications → open the `admin.dockpulse.xyz` app →
   Policies → add a policy with Action=Service Auth and include
   `Service Token = dockpulse-factory-flash`. Without this policy the token
   is rejected at the CF edge before the backend ever sees it.

Sanity-check before flashing a real board:

```bash
curl -X PUT "$DP_BACKEND_URL/api/admin/factory-devices/DP-N-TEST" \
  -H "CF-Access-Client-Id: $DP_CF_ACCESS_CLIENT_ID" \
  -H "CF-Access-Client-Secret: $DP_CF_ACCESS_CLIENT_SECRET" \
  -H "content-type: application/json" \
  -d '{"serial_number":"DP-N-TEST","mesh_uuid":"00000000000000000000000000000001","oob_hex":"00000000000000000000000000000001","claim_jti":"00000000-0000-4000-8000-000000000001","claim_exp":2000000000}'
```

A 200 means edge auth + backend are wired up.

## Per-board flow

```bash
# 1. build the sensor app into build_sensor/
tools/build.sh -r sensor

# 2. plug the board in. port auto-detects to the first /dev/cu.usbmodem*,
#    pass -p /dev/cu.usbmodemXX if multiple boards are attached.
tools/factory-flash.py --serial DP-N-000123
```

That command:

1. Reads the chip MAC, derives the BLE-mesh UUID
   (`MAC ‖ "DOCKPULSE" ‖ 0x01`).
2. Generates a random 16-byte static OOB.
3. Mints a fresh UUID as the sticker jti and assembles `serial:jti` as the
   QR payload.
4. Builds a `factory_nvs` partition image containing the OOB.
5. Erases the chip, flashes bootloader + partition table + app + `factory_nvs`.
6. Writes `device.json`, `qr.txt`, `factory_nvs.bin`, and `qr.png` into
   `factory/devices/DP-N-000123/`.
7. PUTs the device row to
   `$DP_BACKEND_URL/api/admin/factory-devices/DP-N-000123` so adoption can
   look up uuid + oob by serial.

Stick the sticker on the enclosure — the harbor master scans it during PB-ADV
provisioning.

If the script can't reach the backend it fails fast and prints a `curl`
one-liner the operator can rerun once connectivity is back. The board still
ends up flashed, so re-run with `--reflash-nvs` only if the chip got
interrupted mid-flash.

## Variants

| Flag | Use |
| --- | --- |
| `--role gateway` | flash a gateway node instead |
| `--nvs-only` | rotate OOB on an already-flashed board (re-uses the existing app) |
| `--reflash-nvs` | restore `factory_nvs` after `esptool erase_flash` |
| `--force` | overwrite existing artefacts; invalidates the previous sticker AND requires re-registering with the backend |
| `--no-flash` | generate claim + QR + NVS image without touching the board (still POSTs to backend if `DP_BACKEND_URL` is set) |
| `--no-erase` | skip the pre-flash `erase_flash` (faster re-flash, keeps stale NVS) |
| `--backend-url URL` | override `$DP_BACKEND_URL`; pass empty to skip the PUT |
| `--backend-token TOKEN` | bearer token instead of CF Access service token (only useful if Access is disabled for testing) |

## Reprinting a sticker

If the original sticker falls off, the board does not need to be attached:

```bash
tools/print-qr.py --serial DP-N-000123              # writes PNG next to the artefacts
tools/print-qr.py --serial DP-N-000123 --terminal   # ASCII to stdout
```

The QR content is whatever `qr.txt` already holds, so the printed sticker
matches whatever the backend has registered. No backend round-trip needed.

## Re-rolling a claim

If a sticker's `claim_exp` is past or you suspect the QR leaked, run with
`--force`:

```bash
tools/factory-flash.py --serial DP-N-000123 --force --nvs-only
```

`--force` mints a new JTI + expiry, re-POSTs to the backend (overwriting the
existing `factory_devices` row), and produces a new `qr.png`. Any previously
printed sticker for that serial is invalidated by the JTI mismatch check on
adoption.
