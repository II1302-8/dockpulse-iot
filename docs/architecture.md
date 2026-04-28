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
- Single opcode `STATUS_PUB`: `ESP_BLE_MESH_MODEL_OP_3(0x01, 0x02E5)`
  → on the wire `0x01 0xE5 0x02`.
- Payload: 11-byte packed `berth_status_t` (see below).

### Self-provisioning

Every node runs as a single-device PROVISIONER and self-configures via
the local-data API. There is **no on-air provisioning handshake** —
every node is hard-coded with the same NetKey, AppKey, and a
deterministic unicast address derived from `CONFIG_DOCKPULSE_NODE_ID`:

| Role    | NODE_ID | Unicast addr |
| ------- | ------- | ------------ |
| Gateway | 1       | `0x0001`     |
| Sensor  | N       | `0x0001 + N` |

The gateway pre-registers a phantom entry for every possible sensor
address (`0x0002..0x0009`, configurable via `DP_MAX_SENSORS` in
`dp_mesh.c`); each sensor pre-registers the gateway. This bypasses the
`esp_ble_mesh` provisioner's source-address filter that would otherwise
drop all peer traffic — see
[troubleshooting.md](troubleshooting.md#provisioner-src-address-filter)
for the deep dive on why this is needed.

This is deliberately not a production-grade design: shared static keys,
no auth, hardcoded address range, reliance on an internal API
(`bt_mesh_provisioner_provision`). It is appropriate for the course
prototype; replace with standard PB-ADV provisioning before any real
deployment. See [troubleshooting.md](troubleshooting.md#standard-vs-self-provisioning)
for the migration sketch.

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

## Code layout

```
main/                 role dispatch + per-role event loops
  app_main.c          NVS init + role select
  sensor_main.c       sensor loop: radar read → publish
  gateway_main.c      gateway loop: rx callback → uplink

components/dp_common  shared types and codec
  dp_common.h         dp_radar_sample_t, berth_status_t,
                      pack/unpack helpers
  dp_common.c         CONFIG_DOCKPULSE_NODE_ID accessor + codec impl

components/dp_radar   Waveshare HMMD UART driver
  dp_radar.c          frame parser, mode-change command, ACK parsing,
                      fake-radar mode behind CONFIG_DOCKPULSE_RADAR_FAKE

components/dp_mesh    esp_ble_mesh wrapper
  dp_mesh.c           NimBLE host bringup, vendor model registration,
                      self-provisioning, phantom-peer registration

components/dp_gateway uplink (Wi-Fi STA + MQTT)
  dp_gateway.c        berth_status_t → JSON, topic mapping
  dp_gateway_wifi.c   Wi-Fi station bringup
  dp_gateway_mqtt.c   MQTT client + QoS handling
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
