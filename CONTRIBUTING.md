# Contributing

Quick reference for contributors. For deeper material see:

- [docs/architecture.md](docs/architecture.md) — what the firmware does and why
- [docs/development.md](docs/development.md) — toolchain install, helper scripts, bench-testing
- [docs/troubleshooting.md](docs/troubleshooting.md) — known IDF gotchas and how to recover

## Before you push

- Both roles must build cleanly:
  ```bash
  tools/build.sh -r gateway -n 1
  tools/build.sh -r sensor  -n 2 --fake
  ```
- `.clang-format` and `.editorconfig` are checked in — please run them
  before committing. Don't commit `sdkconfig` or `sdkconfig.sensor`
  (both are generated and `.gitignore`d).
- Don't commit `build/` or `build_sensor/`.

## Commit style

We follow Conventional Commits (`feat:`, `fix:`, `chore:`, `docs:`,
etc.). Subject ≤72 chars, body explains _why_ when it isn't obvious.

## PR checklist

- [ ] Both roles build (`tools/build.sh -r gateway -n 1` and
      `tools/build.sh -r sensor -n 2 --fake`).
- [ ] If you touched `dp_mesh`, you bench-tested mesh rx end-to-end
      with two boards and `--fake` sensor.
- [ ] If you touched the wire format (`berth_status_t` or pack/unpack),
      you also updated `docs/architecture.md` and the matching JSON
      mapping in `dp_gateway.c`.
- [ ] If you touched the helper scripts under `tools/`, you also
      updated `docs/development.md`.
- [ ] If you discovered a new IDF gotcha, you added it to
      `docs/troubleshooting.md`.

## Things to be careful about

- `SDKCONFIG_DEFAULTS` is only consulted when the role's sdkconfig is
  generated for the first time. The helper scripts mutate the existing
  sdkconfig in-place via `sync_sdkconfig` in `tools/_common.sh`. If
  you bypass the helpers, edit the sdkconfig directly or delete it.
- The mesh stack's NVS state (RPL, provisioning data) survives across
  reflashes. After a sensor is re-flashed and starts emitting seq=0
  again, the gateway will reject the packets as replays. Use
  `--erase` on the helper scripts (or `idf.py erase-flash`) when in
  doubt.
- We use `esp_ble_mesh`, **not** the NimBLE-mesh port. See
  [docs/troubleshooting.md](docs/troubleshooting.md#mesh-stack-choice)
  before changing anything in `components/dp_mesh/`.
