# Architecture

This document describes what the DockPulse firmware does, how the
sensor and gateway nodes communicate, and the on-the-wire payload
format. For build/flash instructions see [development.md](development.md);
for known IDF issues see [troubleshooting.md](troubleshooting.md).

## Topology

Hub-and-spoke Bluetooth Mesh on ESP32-C3 (ESP-IDF v5.3.2):

```
                  ┌───────────────────────────┐
                  │     Gateway (mains)       │
                  │     addr 0x0001           │
                  │   subscribes 0xC000       │
                  │   uplink → Wi-Fi / MQTT   │
                  └─────────────▲─────────────┘
                                │
                                │  BLE Mesh
                                │  vendor model 0x02E5 / 0x0001
                                │  group 0xC000 (publish)
                                │
        ┌───────────────────────┼───────────────────────┐
        │                       │                       │
┌───────┴───────┐       ┌───────┴───────┐       ┌───────┴───────┐
│    Sensor     │       │    Sensor     │       │    Sensor     │
│  addr 0x0003  │       │  addr 0x0004  │       │  addr 0x0005  │
│   HMMD radar  │       │   HMMD radar  │       │   HMMD radar  │
└───────────────┘       └───────────────┘       └───────────────┘
```

- **One firmware image, two roles.** Selected at build time via
  `CONFIG_DOCKPULSE_ROLE_SENSOR` or `CONFIG_DOCKPULSE_ROLE_GATEWAY` —
  unused-role code is gated with `#if CONFIG_DOCKPULSE_ROLE_*` so it
  doesn't link into the binary.
- **Sensor** = solar-powered, one per berth, mmWave radar (Waveshare
  HMMD), publishes occupancy over BLE Mesh.
- **Gateway** = mains-powered, one per installation, subscribes to a
  group address on the same vendor model, decodes incoming
  `berth_status_t` messages, and forwards them to the cloud over Wi-Fi
  - MQTT.

## Mesh implementation

We use **`esp_ble_mesh`** (Espressif's mesh stack), not the NimBLE-mesh
port that ships in the same IDF tree. Reasons are documented in
[troubleshooting.md](troubleshooting.md#why-not-nimble-mesh).

### Vendor model

- Company ID: `0x02E5` (Espressif — fine for prototypes)
- Model ID: `0x0001`
- Two opcodes:
  - `STATUS_PUB`: `ESP_BLE_MESH_MODEL_OP_3(0x01, 0x02E5)` — 11-byte
    packed `berth_status_t` (single mesh segment).
  - `DIAG_PUB`: `ESP_BLE_MESH_MODEL_OP_3(0x02, 0x02E5)` — 42-byte
    packed `berth_diag_t` (segmented). Opt-in, gated by
    `CONFIG_DOCKPULSE_DIAG_ENABLE`.

### PB-ADV provisioning

Sensor = NODE, gateway = PROVISIONER. Real PB-ADV handshake; on-air keys
+ unicast address are assigned by the gateway during adoption.

Adoption flow:

1. Operator scans the sensor's QR sticker in the dashboard. QR carries a
   factory-signed claim JWT with `uuid`, `oob`, `serial_number`.
2. Backend verifies the JWT, picks a target gateway + berth, publishes
   `dockpulse/v1/gw/{gw_id}/provision/req` (qos 1, retained=false) with
   `{req_id, uuid, oob, ttl_s, berth_id}`.
3. Gateway sets the OOB value, filters scan beacons by UUID, runs
   PB-ADV against the matching unprov sensor. Stack auto-allocates the
   next free unicast in the gateway's `prov_start_address` range.
4. Gateway pushes AppKey Add → Model App Bind → Model Pub Set via cfg
   client to the new node, retrieves its DevKey, then publishes
   `dockpulse/v1/gw/{gw_id}/provision/resp` with
   `{req_id, status: "ok", unicast_addr, dev_key_fp}` (sha256(dev_key)
   first 8 bytes hex). Errors yield `{status: "err", code, msg}`.
5. Backend writes a `Node` row keyed on `unicast_addr` + `berth_id`.
   Gateway records the same mapping locally in dp_prov NVS; uplink
   uses it to render the MQTT topic.

Gateway online/offline state: gateway publishes
`dockpulse/v1/gw/{gw_id}/status` retained `{online: true}` on connect,
LWT publishes `{online: false}` on disconnect.

Hot-swap: re-running step 1 with a new sensor on a berth that already
has a Node row replaces the gateway's old `unicast -> berth` mapping in
dp_prov; the new node takes over publishing. Backend Node row is
similarly replaced (server-side ticket).

Factory reset: 5-second long-press on GPIO 9 wipes both dp_prov and
the BLE Mesh stack NVS, then reboots into PB-ADV unprov mode.

Bench / two-board testing: when no real backend is around, ship-time
`CONFIG_DOCKPULSE_NODE_ID` still works as a payload `node_id` and
fallback berth-id index — bring up gateway + sensor, leave gateway in
`UPLINK_STUB=y` to skip MQTT.

## Wire format

`berth_status_t` is the canonical sensor → gateway message. The same
struct is also serialized to JSON by the gateway for MQTT.

### Struct (defined in `components/dp_common/include/dp_common.h`)

```c
typedef struct {
    uint8_t  node_id;        // 1..255, sensor's CONFIG_DOCKPULSE_NODE_ID
    uint16_t berth_id;       // logical berth id (currently = mesh src addr)
    bool     occupied;       // true = boat present
    uint16_t sensor_raw_mm;  // raw range in millimetres
    uint8_t  battery_pct;    // 0..100, or DP_BATTERY_UNKNOWN (0xFF)
    uint32_t ts_ms;          // sender's monotonic ms (gateway stamps wallclock on uplink)
} berth_status_t;
```

### Mesh wire layout

11 bytes, little-endian. Fits in a single mesh segment
(`BT_MESH_APP_SEG_SDU_MAX = 12`).

```
offset  size  field
------  ----  ---------------------------------------
  0      1   node_id
  1      2   berth_id (LE)
  3      1   occupied (0 / 1)
  4      2   sensor_raw_mm (LE)
  6      1   battery_pct (0xFF = unknown)
  7      4   ts_ms (LE)
```

`berth_status_pack()` and `berth_status_unpack()` in `dp_common.c` are
the bounds-checked codec.

### Diagnostic struct (`berth_diag_t`)

Carries raw radar internals for dashboard-side debugging. Defined in
the same header.

```c
typedef struct {
    uint8_t  node_id;
    uint16_t berth_id;
    int8_t   target_state;     // raw radar state byte
    uint16_t raw_distance_cm;  // pre-filter distance reported by radar
    uint16_t gate_energy[16];  // per-gate magnitude, ~70 cm per gate
    uint32_t ts_ms;
} berth_diag_t;
```

Wire layout — 42 bytes, little-endian:

```
offset  size  field
------  ----  ---------------------------------------
  0      1   node_id
  1      2   berth_id (LE)
  3      1   target_state
  4      2   raw_distance_cm (LE)
  6     32   gate_energy[0..15] (16 × LE u16)
 38      4   ts_ms (LE)
```

Codec: `berth_diag_pack()` / `berth_diag_unpack()` in `dp_common.c`.
Larger than the 12-byte unsegmented SDU, so the message uses BLE Mesh's
segmented transport (~6 segments). Sent on the `DIAG_PUB` opcode and
forwarded by the gateway on the `harbor/.../diag` MQTT topic.

### MQTT JSON

The gateway converts `berth_status_t` to the JSON schema documented in
`docs/api/mqtt-contract.yml` (in the parent `dockpulse` repo, not this
firmware repo) under topic
`harbor/{harbor_id}/{dock_id}/{berth_id}/status` (default
`harbor/main/dock-a/berth-002/status`). `harbor_id` and `dock_id` are
configurable via `CONFIG_DOCKPULSE_HARBOR_ID` and
`CONFIG_DOCKPULSE_DOCK_ID`; the backend's parser (`backend/app/mqtt.py`)
requires exactly five `/`-separated segments starting with `harbor/`,
so the prefix shape is mandatory.

Example payload:

```json
{
  "node_id": "node-002",
  "berth_id": "berth-002",
  "occupied": true,
  "sensor_raw": 2000,
  "battery_pct": 87,
  "timestamp": "2026-04-28T14:30:00Z"
}
```

`battery_pct` is omitted when the sensor reports `DP_BATTERY_UNKNOWN`
(no battery monitoring on the current HW rev). `timestamp` is ISO 8601
UTC from the gateway's clock — pre-SNTP it'll emit a 1970-epoch stamp.

#### Diag topic

When `CONFIG_DOCKPULSE_DIAG_ENABLE=y` (default) the gateway also
publishes a JSON diagnostics message on
`harbor/{harbor_id}/{dock_id}/{berth_id}/diag` once per status period.
Same topic shape as `status` so the backend's `_parse_berth_topic`
accepts it; backend storage of the new `kind="diag"` is tracked
separately.

```json
{
  "node_id": "node-002",
  "berth_id": "berth-002",
  "target_state": 1,
  "raw_distance_cm": 230,
  "gate_energy": [0, 0, 0, 16384, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  "timestamp": "2026-04-28T14:30:00Z"
}
```

`gate_energy` is a fixed 16-element array of unsigned LE u16 magnitudes
straight from the HMMD's Report frame; units are firmware-internal —
treat as relative magnitudes for thresholding. `target_state` is the
HMMD presence/state byte (currently mirrors `presence`). Diag is
intended for debugging misbehaving nodes from the dashboard, not for
routine operation.

## Code layout

```
main/                 role dispatch + per-role event loops
  app_main.c          NVS init + LED/button + role select
  sensor_main.c       sensor loop: radar read → publish (gated on adoption)
  gateway_main.c      gateway loop: rx callback → uplink

components/dp_common  shared types + codec
components/dp_radar   Waveshare HMMD UART driver
components/dp_mesh    esp_ble_mesh wrapper, PB-ADV adoption
components/dp_prov    NVS unicast→berth_id map + factory reset
components/dp_io      LED state machine + factory-reset button
components/dp_gateway Wi-Fi + MQTT, MQTT adoption topics
  dp_gateway_adopt.c  provision/req → mesh provision → provision/resp
```

## Sensor periodicity

- Default sensor period: `CONFIG_DOCKPULSE_SENSOR_PERIOD_MS = 60000`
  (60 s). Override in `menuconfig` or by appending the line to the
  role's sdkconfig.
- Radar frames stream at ~10 Hz from the HMMD module; we drain one
  frame per sensor period and ignore the rest.
- Future work: light/deep sleep between samples for solar power
  budget. Currently `vTaskDelay`.

## Gateway uplink

- Wi-Fi STA in `dp_gateway_wifi.c` — credentials in Kconfig.
- MQTT client in `dp_gateway_mqtt.c` — broker URI in Kconfig.
- `CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB=y` (default) logs uplink
  intent without touching network — useful for bench-testing the mesh
  without a broker.
- `CONFIG_DOCKPULSE_MQTT_SELFTEST=y` makes the gateway publish one
  synthetic `berth_status_t` after broker connect, useful for
  smoke-testing the broker integration in isolation.
