# Development

How to set up the toolchain, build, flash, monitor, and bench-test the
firmware. For what the firmware actually does see
[architecture.md](architecture.md); for known issues see
[troubleshooting.md](troubleshooting.md).

## Toolchain

Targets **ESP-IDF v5.3.2** for ESP32-C3.

### One-time install

```bash
brew install cmake ninja dfu-util ccache libusb python
git clone -b v5.3.2 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32c3
```

### Activate per shell

```bash
. $HOME/esp/esp-idf/export.sh
```

Tip: add `alias get_idf='. $HOME/esp/esp-idf/export.sh'` to `~/.zshrc`.
The helper scripts under `tools/` source ESP-IDF for you, so you only
need to activate manually when invoking `idf.py` directly.

## Per-role build dirs and sdkconfigs

The project supports two roles (sensor and gateway) from a single
source tree. Each role has its own build directory and its own
sdkconfig file so they don't trample each other:

| Role    | Build dir       | Sdkconfig file       |
| ------- | --------------- | -------------------- |
| Gateway | `build/`        | `sdkconfig`          |
| Sensor  | `build_sensor/` | `sdkconfig.sensor`   |

Both sdkconfig files are seeded from `sdkconfig.defaults` on first
build. Neither should be committed (both are in `.gitignore`).

## Helper scripts

All helpers live under `tools/`. They source ESP-IDF, parse a uniform
flag set, and pick the right build dir / sdkconfig per role. Run any
of them with `--help` for a usage summary.

### Common flags

| Flag                       | Meaning                                                            |
| -------------------------- | ------------------------------------------------------------------ |
| `-r, --role gateway|sensor`| Which role to build/flash. Default `gateway`.                      |
| `-p, --port PATH`          | Serial port. Default = first `/dev/cu.usbmodem*`.                  |
| `-n, --node-id N`          | `CONFIG_DOCKPULSE_NODE_ID` (1..255). Sticky — written to sdkconfig.|
| `--fake` / `--real`        | Sensor only. Toggle `CONFIG_DOCKPULSE_RADAR_FAKE`.                 |
| `--erase`                  | `flash`/`run` only. `erase-flash` before flashing — wipes NVS.     |

The role/node-id/fake flags are applied to the role's sdkconfig on
**every** invocation (via `sync_sdkconfig` in `tools/_common.sh`),
which is why role swaps don't need a `fullclean` between them.

### `tools/build.sh`

Builds firmware for one role into the role's build dir. Examples:

```bash
tools/build.sh -r gateway -n 1
tools/build.sh -r sensor  -n 2 --fake     # bench-test, no radar wired
tools/build.sh -r sensor  -n 3 --real     # real HMMD attached
```

### `tools/flash.sh`

Build + flash. Refuses to run if no port is detected and none was
passed.

```bash
tools/flash.sh -r gateway -p /dev/cu.usbmodem21
tools/flash.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake --erase
```

### `tools/monitor.sh`

Open serial monitor on a port. Use `-r` only if you want symbol
decoding from the matching ELF; otherwise it's a pure UART tap.

```bash
tools/monitor.sh -p /dev/cu.usbmodem21
tools/monitor.sh -p /dev/cu.usbmodem21 -r gateway
```

Exit with `Ctrl-]`.

### `tools/run.sh`

Build + flash + monitor in one shot.

```bash
tools/run.sh -r gateway -p /dev/cu.usbmodem21
tools/run.sh -r sensor  -p /dev/cu.usbmodem11 -n 2 --fake --erase
```

### `tools/list-ports.sh`

Lists attached USB-serial ports with whatever metadata the OS exposes
(`system_profiler` on macOS, `udevadm` on Linux). Use this before
passing `-p` when you have multiple boards plugged in — port numbering
can drift across reboots/replug.

```bash
tools/list-ports.sh
# /dev/cu.usbmodem21    Manufacturer=Espressif; Serial=0E5C8...
# /dev/cu.usbmodem11    Manufacturer=Espressif; Serial=2B19A...
```

## Two-board workflow

Typical bench setup with one gateway and one sensor on separate boards:

```bash
# Find which port is which
tools/list-ports.sh

# Terminal 1: gateway
tools/run.sh -r gateway -p /dev/cu.usbmodem21 -n 1 --erase

# Terminal 2: sensor (no radar attached)
tools/run.sh -r sensor -p /dev/cu.usbmodem11 -n 2 --fake --erase
```

Each terminal stays attached to its own board. Use `--erase` on first
flash of any new firmware that touches mesh state — clears the mesh
NVS, in particular the replay-protection list (RPL) which would
otherwise reject low-seq packets from re-flashed sensors. See
[troubleshooting.md](troubleshooting.md#rpl-replay).

## Manual `idf.py` invocations

If you bypass the helpers, you must pass `-B` and `-DSDKCONFIG=` for
the sensor role:

```bash
# Gateway (uses default build/, sdkconfig)
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbmodem21 flash monitor

# Sensor — explicit -B and -DSDKCONFIG
idf.py -B build_sensor -DSDKCONFIG=sdkconfig.sensor menuconfig
idf.py -B build_sensor -DSDKCONFIG=sdkconfig.sensor build
idf.py -B build_sensor -DSDKCONFIG=sdkconfig.sensor -p /dev/cu.usbmodem11 flash monitor
```

`SDKCONFIG_DEFAULTS` only seeds an sdkconfig on first generation. Once
a build dir's sdkconfig exists, the defaults file is ignored — that's
why the helper scripts write Kconfig values directly via
`sync_sdkconfig` rather than relying on defaults.

## Verifying both roles build

CI and reviewers should make sure both roles compile cleanly:

```bash
tools/build.sh -r gateway -n 1
tools/build.sh -r sensor  -n 2 --fake
```

## Bench-testing without hardware

The HMMD radar can be skipped at compile time:

```bash
tools/run.sh -r sensor -p /dev/cu.usbmodem11 -n 2 --fake
```

`--fake` enables `CONFIG_DOCKPULSE_RADAR_FAKE`, which makes
`dp_radar_read()` return synthetic samples (distance walks 200..780 cm
in a 30-step cycle, fake gate-energy peak at the matching gate). This
exercises the mesh + gateway path end-to-end without touching UART or
needing any sensor hardware.

To test the gateway → MQTT path against a local broker:

```bash
brew install mosquitto && brew services start mosquitto
mosquitto_sub -v -t 'dockpulse/#'
```

Configure the gateway's sdkconfig:

```
CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB=n
CONFIG_DOCKPULSE_WIFI_SSID=...
CONFIG_DOCKPULSE_WIFI_PASSWORD=...
CONFIG_DOCKPULSE_MQTT_BROKER_URI=mqtt://192.168.x.y:1883
```

For an end-to-end JSON smoke test without any sensor:

```
CONFIG_DOCKPULSE_MQTT_SELFTEST=y
```

The gateway will publish one synthetic `berth_status_t` after MQTT
connect.

## Field-test data capture

The sensor logs one CSV-style line per radar frame at INFO level so
logs can be grepped for plotting. Format (see `main/sensor_main.c`):

```
RADAR,<ts_ms>,<presence>,<distance_cm>,<gate0>,<gate1>,...,<gate15>
```

Capture with:

```bash
tools/monitor.sh -p /dev/cu.usbmodem11 | grep ',RADAR,' > field_test.csv
```

Then plot in pandas/gnuplot. Use this to set per-berth gate-energy
thresholds before writing the boat-detection logic.

## Code style

`.clang-format` and `.editorconfig` are checked in. Run them before
committing. `sdkconfig` and `sdkconfig.sensor` are generated and
`.gitignore`d; don't commit them.
