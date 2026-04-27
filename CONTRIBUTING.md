# Contributing

## Toolchain

Targets **ESP-IDF v5.3.2** for ESP32-C3. One-time install:

```bash
brew install cmake ninja dfu-util ccache libusb python
git clone -b v5.3.2 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32c3
```

Activate per shell:

```bash
. $HOME/esp/esp-idf/export.sh
```

Tip — add `alias get_idf='. $HOME/esp/esp-idf/export.sh'` to `~/.zshrc`.

## Build

```bash
idf.py menuconfig          # DockPulse → Node role
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Convenience wrappers (auto-source ESP-IDF, auto-detect port):

```bash
tools/flash.sh             # build + flash
tools/monitor.sh           # serial monitor (Ctrl-] to exit)
tools/run.sh               # build + flash + monitor
```

Pass an explicit port as the first argument if multiple devices are
attached: `tools/run.sh /dev/cu.usbmodem101`.

Default role is sensor. For gateway, toggle in `menuconfig` or set
`CONFIG_DOCKPULSE_ROLE_GATEWAY=y` in `sdkconfig`.

Sensor- and gateway-only sources are gated with
`#if CONFIG_DOCKPULSE_ROLE_SENSOR` / `_GATEWAY`, so the unused role's
code does not link into the image. Both roles must build green —
verify with:

```bash
rm -f sdkconfig && idf.py build                                       # sensor
printf 'CONFIG_DOCKPULSE_ROLE_SENSOR=n\nCONFIG_DOCKPULSE_ROLE_GATEWAY=y\n' \
    >> sdkconfig && idf.py build                                       # gateway
```

## Layout

```
main/                 role dispatch + per-role event loops
components/dp_common  shared types
components/dp_radar   HMMD UART driver
components/dp_mesh    NimBLE Mesh wrapper
components/dp_gateway uplink (Wi-Fi STA + MQTT publish)
tools/                dev convenience scripts (flash, monitor, run)
```

## Code style

`.clang-format` and `.editorconfig` are checked in — please run them
before committing. Don't commit `sdkconfig` (generated).
