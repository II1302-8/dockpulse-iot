# Troubleshooting

Known issues, gotchas, and workarounds for ESP-IDF v5.3.2 + esp_ble_mesh
+ DockPulse firmware. If you hit something not listed here, file an
issue and add it to this doc.

## Mesh stack choice

### Why not NimBLE-mesh?

ESP-IDF v5.3.2 ships with **two** Bluetooth Mesh stacks:

1. **NimBLE-mesh** — `components/bt/host/nimble/.../host/mesh/`. Part of the NimBLE host port.
2. **`esp_ble_mesh`** — `components/bt/esp_ble_mesh/`. Espressif's own stack, runs on either NimBLE or Bluedroid host.

We use **`esp_ble_mesh`**. The NimBLE-mesh FreeRTOS port is incomplete:

- Per-buffer adv events in the mesh's adv pool are never `ble_npl_event_init`'d. `bt_mesh_adv_create_from_pool` only calls `ble_npl_event_set_arg`, which silently no-ops when `ev->event` is NULL (see `npl_os_freertos.c:486`). Result: first publish hits `assert(ble_npl_event_get_arg(ev))` in `glue.c:84` and panics.
- The legacy adv worker thread that drains `bt_mesh_adv_queue` is gated `#ifdef MYNEWT` (see `adv_legacy.c:225`) — never started on FreeRTOS.
- The `bt_mesh_adv_update()` path in `bt_mesh_start()` queues a static `ble_npl_event` that was never initialized. Triggering `BT_MESH_GATT_PROXY=y` makes `bt_mesh_start()` walk that path on first provisioning, causing a load-fault panic before any application code runs.

The official IDF `blemesh` example only does init + sit idle, never exercising publish/subscribe — so the bug surface never trips there.

Switch is in `sdkconfig.defaults`:

```
# CONFIG_BT_NIMBLE_MESH is not set
CONFIG_BLE_MESH=y
CONFIG_BLE_MESH_NODE=y
CONFIG_BLE_MESH_PROVISIONER=y
```

## esp_ble_mesh API gotchas

These all bit us during development. Documented here so the next person hitting them finds the fix in 30 seconds instead of 30 minutes.

### `prov_start_address = 0xFFFF` is rejected

`esp_ble_mesh_prov_t.prov_start_address` is validated at `provisioner_prov_enable` time. `0xFFFF` is reserved/group-range and fails with `Invalid address, own 0x0001, start 0xffff`. Use `0x7FFF` (top of unicast range) — required even when you never onboard another device.

### `add_local_net_key` rejects `net_idx = 0`

`esp_ble_mesh_provisioner_add_local_net_key` explicitly returns `ESP_ERR_INVALID_ARG` for `net_idx == ESP_BLE_MESH_KEY_PRIMARY` (0x0000). The primary subnet is auto-created with a random key when the provisioner is enabled — to install a deterministic shared key on top, use `esp_ble_mesh_provisioner_update_local_net_key(NET_KEY, 0)` instead.

### `bind_app_key_to_local_model` argument order differs by role

Two functions, easy to confuse:

| API                                                       | Argument order                              |
| --------------------------------------------------------- | ------------------------------------------- |
| `esp_ble_mesh_node_bind_app_key_to_local_model`           | `(elem_addr, company_id, model_id, app_idx)` |
| `esp_ble_mesh_provisioner_bind_app_key_to_local_model`    | `(elem_addr, app_idx, model_id, company_id)` |

We use the provisioner one. Wrong arg order silently logs `No model found, model id 0x0001, cid 0x0000` and the bind fails with `err=-19`.

### Provisioner src-address filter

`core/net.c:1885` drops every received packet whose source unicast isn't in the provisioner's known-nodes DB:

```c
if (!bt_mesh_provisioner_get_node_with_addr(src) &&
    !bt_mesh_elem_find(src)) {
    BT_INFO("Not found node address 0x%04x", src);
    return true;  // discard
}
```

Self-provisioning leaves the DB empty. Symptom: gateway logs `appkey bound`, `model subscribed`, then receives nothing — sensor's `publish comp err=0` succeeds but the gateway never fires `model rx`. With NimBLE log level dropped to WARNING the discard message is invisible too.

**Workaround we use:** call the internal-but-callable `bt_mesh_provisioner_provision()` directly to stuff phantom entries into the node DB. See `register_phantom_peer()` in `dp_mesh.c`. Each sensor pre-registers the gateway addr; the gateway pre-registers the full sensor addr range (`DP_GATEWAY_ADDR + 1 .. + DP_MAX_SENSORS`). The placeholder dev_key is fine because we only send appkey-encrypted (not devkey-encrypted) messages between nodes.

### RPL replay

After the src filter accepts a packet, the **replay protection list** kicks in. If the gateway has previously seen any seq from a given src, it rejects everything `<=` that seq. Symptom on gateway:

```
W BLE_MESH: Replay: src 0x0003 dst 0xc000 seq 0x000000
W BLE_MESH: Replay: src 0x0003 dst 0xc000 seq 0x000001
...
```

The RPL persists in NVS. After re-flashing a sensor with cleared NVS its seq counter restarts at 0, but the gateway's RPL still remembers the old high-water mark.

**Fix:** `idf.py erase-flash` on the gateway (or both boards). The helper scripts have `--erase` to do this in one shot:

```bash
tools/run.sh -r gateway -p /dev/cu.usbmodem21 -n 1 --erase
```

In production this won't matter — sensors don't re-flash and the gateway's RPL grows monotonically with the real sensor's seq counter.

## NimBLE GAP log spam

ESP-IDF logs every advertise stop/start at INFO level. The mesh stack toggles advertising every ~100 ms which buries application logs in NimBLE GAP chatter. Fixed in `sdkconfig.defaults`:

```
# CONFIG_BT_NIMBLE_LOG_LEVEL_INFO is not set
CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
```

If the existing sdkconfig in your build dir was created before this change, the defaults file is ignored on subsequent runs. Patch in place:

```bash
sed -i.bak \
    -e 's|^CONFIG_BT_NIMBLE_LOG_LEVEL_INFO=y|# CONFIG_BT_NIMBLE_LOG_LEVEL_INFO is not set|' \
    -e 's|^# CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING is not set|CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y|' \
    sdkconfig sdkconfig.sensor
rm -f sdkconfig.bak sdkconfig.sensor.bak
```

Then rebuild.

## sdkconfig & SDKCONFIG_DEFAULTS

`-DSDKCONFIG_DEFAULTS=...` only seeds `<build_dir>/<sdkconfig>` the **first time** that sdkconfig is generated. Once it exists, IDF reuses it and silently ignores any defaults file. This caused real confusion when `tools/run.sh -r sensor` appeared to do nothing — the existing sdkconfig already had `CONFIG_DOCKPULSE_ROLE_GATEWAY=y` from an earlier build.

Helper scripts now mutate the existing sdkconfig directly via `sync_sdkconfig` in `tools/_common.sh` — every invocation enforces the requested role / node_id / fake-radar settings.

If you bypass the helpers and edit `sdkconfig.defaults`, either delete the existing sdkconfig or hand-edit it for the change to take effect.

## Standard vs self-provisioning

The phantom-peer trick is a working hack, not the textbook pattern. Pros: code is symmetric across roles, sensors and gateway both run the same self-config flow. Cons: relies on an internal IDF function, hardcoded sensor address range, zero dev_key per peer.

The standard pattern would have:

- Gateway as PROVISIONER + NODE.
- Sensors as NODE only, calling `esp_ble_mesh_node_prov_enable(PROV_ADV)` to broadcast unprovisioned beacons.
- Gateway receives `ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT`, calls `esp_ble_mesh_provisioner_prov_device_with_addr(uuid, NULL, 0, PROV_ADV, NET_IDX, derived_addr)`. Real PB-ADV protocol runs.
- After `PROVISIONER_PROV_COMPLETE_EVT`, gateway sends config-client messages: `app_key_add` → `model_app_bind` → `model_pub_set`.

Cost: ~200 extra lines, mostly state-machine boilerplate. Worth doing before any non-prototype deployment.

## Common build/flash errors

### `Cannot open file '...build/config/sdkconfig.h'`

Reported by clangd when the build dir doesn't exist yet. Run `tools/build.sh -r ROLE` once and clangd will pick up the generated header.

### `Unsupported argument 'rv32imc_zicsr_zifencei' to option '-march='`

clangd noise — clangd is reading `compile_commands.json` with its own clang, not the riscv32-esp-elf-gcc actually used by the build. Ignore. The IDF build is the truth.

### `idf.py: command not found`

You forgot to source ESP-IDF. The helper scripts under `tools/` do this for you; manual invocations need `. $HOME/esp/esp-idf/export.sh`.
