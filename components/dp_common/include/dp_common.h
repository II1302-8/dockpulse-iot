#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP_RADAR_GATE_COUNT 16
#define DP_RADAR_GATE_CM    70 // each gate spans ~70 cm of range

typedef struct {
    bool presence;
    uint16_t distance_cm;
    int8_t target_state; // raw radar state byte; sensor-specific meaning
    uint32_t ts_ms;      // monotonic ms at sample time
    // Per-gate magnitude from HMMD Report mode. Index 0 = nearest gate
    // (0..70 cm), index 15 = farthest (~10.5..11.2 m). Format is the
    // module's own LE uint16 — units are firmware-internal, treat as
    // relative magnitudes for thresholding.
    uint16_t gate_energy[DP_RADAR_GATE_COUNT];
} dp_radar_sample_t;

// berth_status_t is the on-the-wire status message that sensor nodes
// publish to the gateway over BLE mesh. The gateway translates it to
// the JSON schema documented in docs/api/mqtt-contract.yml under
// `harbor/.../status`. Field names mirror that contract.
//
// `battery_pct == DP_BATTERY_UNKNOWN` (0xFF) means the sender did not
// measure or did not have a battery (e.g. mains-powered test rig).
typedef struct {
    uint8_t node_id;        // sensor's CONFIG_DOCKPULSE_NODE_ID, 1..255
    uint16_t berth_id;      // logical berth id (currently = mesh src addr)
    bool occupied;          // true = boat present
    uint16_t sensor_raw_mm; // raw range in millimetres (matches contract `sensor_raw`)
    uint8_t battery_pct;    // 0..100, or DP_BATTERY_UNKNOWN
    uint32_t ts_ms;         // sender's monotonic ms; gateway stamps wallclock on uplink
} berth_status_t;

#define DP_BATTERY_UNKNOWN    0xFF
#define BERTH_STATUS_WIRE_LEN 11 // node_id(1)+berth_id(2)+occupied(1)+raw(2)+batt(1)+ts(4)

esp_err_t dp_common_get_node_id(uint8_t *out);

// Pack berth_status_t into a little-endian byte buffer. `cap` must be
// >= BERTH_STATUS_WIRE_LEN; `out_len` (if non-NULL) is set to the
// number of bytes written.
esp_err_t berth_status_pack(const berth_status_t *s, uint8_t *buf, size_t cap, size_t *out_len);

// Unpack `len` bytes back into a berth_status_t. `len` must be at
// least BERTH_STATUS_WIRE_LEN; trailing bytes are ignored.
esp_err_t berth_status_unpack(const uint8_t *buf, size_t len, berth_status_t *out);

#ifdef __cplusplus
}
#endif
