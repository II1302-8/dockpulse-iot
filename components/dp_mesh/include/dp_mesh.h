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

// sensor cb. fires after PB-ADV completes or NVS restore. publish ok after
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

// --- gateway adoption API ---
// unicast_addr/dev_key only set on ok
typedef struct {
    bool ok;
    uint16_t unicast_addr;
    uint8_t dev_key[16];
    const char *err_code; // e.g. "timeout", "scan-miss", "bind-fail"
    const char *err_msg;
} dp_mesh_prov_result_t;

typedef void (*dp_mesh_prov_done_cb_t)(const dp_mesh_prov_result_t *res, void *ctx);

// scan for unprov beacon matching uuid. run PB-ADV. push AppKey + pub via
// cfg client. cb fires once. one flow at a time else ESP_ERR_INVALID_STATE.
// static_oob is 16 bytes from backend QR JWT. NULL for OOB-less prototype
esp_err_t dp_mesh_gateway_provision(const uint8_t uuid[16], const uint8_t *static_oob,
                                    uint32_t timeout_ms, dp_mesh_prov_done_cb_t cb, void *ctx);

// drop node from provisioner table. frees the unicast slot for reuse
esp_err_t dp_mesh_gateway_delete_node(uint16_t unicast_addr);

// true if the gateway already provisioned a node with this UUID. used to
// short-circuit re-adoption with code=already-provisioned instead of a 180s
// scan that's guaranteed to fail (the node won't beacon while provisioned)
bool dp_mesh_gateway_has_node_with_uuid(const uint8_t uuid[16]);

// fires at each provisioning phase transition so the adopt layer can publish
// progress over MQTT. state is one of: "started", "link-open", "pb-adv-done",
// "cfg-app-key", "cfg-bind", "cfg-pub-set", "complete".
typedef void (*dp_mesh_prov_state_cb_t)(const char *state, void *ctx);
void dp_mesh_gateway_set_state_cb(dp_mesh_prov_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
