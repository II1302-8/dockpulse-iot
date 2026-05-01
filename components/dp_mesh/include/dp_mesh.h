#pragma once

#include "dp_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DP_MESH_ROLE_SENSOR,
    DP_MESH_ROLE_GATEWAY,
} dp_mesh_role_t;

typedef void (*dp_mesh_status_handler_t)(const berth_status_t *s, uint16_t src_addr);
typedef void (*dp_mesh_diag_handler_t)(const berth_diag_t *d, uint16_t src_addr);

// Sensor: fired when PB-ADV completes (first boot) or when stack
// finishes restoring from NVS (subsequent boots). After this fires the
// sensor may publish.
typedef void (*dp_mesh_sensor_ready_cb_t)(uint16_t unicast_addr);

typedef struct {
    dp_mesh_role_t role;
    dp_mesh_status_handler_t status_cb;     // gateway only
    dp_mesh_diag_handler_t diag_cb;         // gateway only
    dp_mesh_sensor_ready_cb_t sensor_ready; // sensor only
} dp_mesh_cfg_t;

esp_err_t dp_mesh_init(const dp_mesh_cfg_t *cfg);
esp_err_t dp_mesh_publish_status(const berth_status_t *s);
esp_err_t dp_mesh_publish_diag(const berth_diag_t *d);

// --- Gateway-only adoption API ---
//
// Result of a provision attempt. unicast_addr/dev_key only set on ok.
typedef struct {
    bool ok;
    uint16_t unicast_addr;
    uint8_t dev_key[16];
    const char *err_code; // e.g. "timeout", "scan-miss", "bind-fail"
    const char *err_msg;
} dp_mesh_prov_result_t;

typedef void (*dp_mesh_prov_done_cb_t)(const dp_mesh_prov_result_t *res, void *ctx);

// Start provisioning a sensor identified by mesh UUID. The gateway scans
// for a matching unprov beacon, runs PB-ADV, then binds AppKey + sets
// publication on the new node's vendor model. cb fires once with the
// outcome. Only one provision flow at a time — returns ESP_ERR_INVALID_STATE
// if another is in flight.
//
// `static_oob` is the 16-byte value the backend extracted from the QR
// claim JWT. Pass NULL for OOB-less provisioning (prototype testing).
esp_err_t dp_mesh_gateway_provision(const uint8_t uuid[16], const uint8_t *static_oob,
                                    uint32_t timeout_ms, dp_mesh_prov_done_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
