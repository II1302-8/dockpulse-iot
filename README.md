# DockPulse IoT

Firmware for the DockPulse berth-occupancy network. KTH **II1302**, team _8-mile_.

One ESP-IDF project, two roles selected at build time:

| Role        | Power | Job                                                                 |
| ----------- | ----- | ------------------------------------------------------------------- |
| **Sensor**  | Solar | mmWave radar per berth, publishes presence + distance via BLE Mesh. |
| **Gateway** | Mains | One per site. Subscribes to sensors and forwards to the cloud.      |

## Hardware

- **MCU**: ESP32-C3, 4 MB flash.
- **Radar**: [Waveshare HMMD mmWave](https://www.waveshare.com/wiki/HMMD_mmWave_Sensor) (UART 115200 8N1).

Default wiring (override in `menuconfig`):

| HMMD | ESP32-C3         |
| ---- | ---------------- |
| 3V3  | 3V3              |
| GND  | GND              |
| TX   | GPIO 5 (UART RX) |
| RX   | GPIO 4 (UART TX) |

## Building

See [CONTRIBUTING.md](CONTRIBUTING.md) for toolchain install, build, and flash instructions.
