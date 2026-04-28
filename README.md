# DockPulse IoT

Firmware for the DockPulse berth-occupancy network. KTH **II1302**, team _8-mile_.

One ESP-IDF project, two roles selected at build time:

| Role        | Power | Job                                                                       |
| ----------- | ----- | ------------------------------------------------------------------------- |
| **Sensor**  | Solar | mmWave radar per berth, publishes occupancy + distance via BLE Mesh.      |
| **Gateway** | Mains | One per site. Subscribes to sensors, forwards `berth_status_t` over MQTT. |

## Hardware

- **MCU**: ESP32-C3, 4 MB flash.
- **Radar**: [Waveshare HMMD mmWave](https://www.waveshare.com/wiki/HMMD_mmWave_Sensor) (UART 115200 8N1).

Default sensor wiring (override in `menuconfig`):

| HMMD | ESP32-C3         |
| ---- | ---------------- |
| 3V3  | 3V3              |
| GND  | GND              |
| TX   | GPIO 5 (UART RX) |
| RX   | GPIO 4 (UART TX) |

## Quick start

```bash
# One-time toolchain install — see docs/development.md
~/esp/esp-idf/install.sh esp32c3

# Bench test with no radar attached: gateway on one board, fake-radar
# sensor on another. List ports first if multiple boards plugged in.
tools/list-ports.sh
tools/run.sh -r gateway -p /dev/cu.usbmodem21 -n 1 --erase
tools/run.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake --erase
```

Within ~minute the gateway's monitor should show:

```
gateway: rx src=0x0003 node=2 berth=2 occupied=1 raw_mm=2000
dp_gateway: uplink-stub src=0x0003 node=2 berth=2 occupied=1 raw_mm=2000 ts=...
```

## Documentation

- [docs/architecture.md](docs/architecture.md) — what the firmware does, mesh topology, wire format, code layout
- [docs/development.md](docs/development.md) — toolchain, build/flash/monitor helpers, two-board workflow, field-test capture
- [docs/troubleshooting.md](docs/troubleshooting.md) — known IDF v5.3.2 gotchas, mesh stack quirks, common errors
- [CONTRIBUTING.md](CONTRIBUTING.md) — code style, PR process
