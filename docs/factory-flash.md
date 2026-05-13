# Factory-flashing a new sensor node

Each board needs an app image, a per-device OOB blob in `factory_nvs`, a signed
adoption JWT, and a QR sticker. `tools/factory-flash.py` handles all of it in
one shot.

## Host setup (once per workstation)

```bash
tools/factory-keygen.sh        # generates the factory Ed25519 keypair
brew install qrencode          # needed for the sticker PNG
```

## Per-board flow

```bash
# 1. build the sensor app into build_sensor/
tools/build.sh -r sensor

# 2. plug the board in. port auto-detects to the first /dev/cu.usbmodem*,
#    pass -p /dev/cu.usbmodemXX if multiple boards are attached.
tools/factory-flash.py --serial DP-N-000123
```

That command reads the chip MAC, derives the BLE-mesh UUID, generates a random
16-byte static OOB, signs an adoption JWT, flashes bootloader + partition table
+ app + `factory_nvs`, and writes the QR PNG and metadata to
`factory/devices/DP-N-000123/`. Stick the sticker on the enclosure — the
gateway operator scans it during PB-ADV provisioning.

## Variants

| Flag | Use |
| --- | --- |
| `--role gateway` | flash a gateway node instead |
| `--nvs-only` | rotate OOB on an already-flashed board (re-uses the existing app) |
| `--reflash-nvs` | restore `factory_nvs` after `esptool erase_flash` |
| `--force` | overwrite existing artefacts (invalidates the previous sticker) |
| `--no-flash` | generate JWT + QR + NVS image without touching the board |

## Reprinting a sticker

If the original sticker falls off, the board does not need to be attached:

```bash
tools/print-qr.py --serial DP-N-000123              # writes PNG next to the artefacts
tools/print-qr.py --serial DP-N-000123 --terminal   # ASCII to stdout
```
