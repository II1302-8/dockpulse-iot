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

// Status callback fired on the gateway when a sensor's berth_status_t
// is received. `src_addr` is the mesh unicast address of the sender —
// useful for diagnostics; the logical berth id lives in `s->berth_id`
typedef void (*dp_mesh_status_handler_t)(const berth_status_t *s, uint16_t src_addr);

// gateway rx callback for berth_diag_t
typedef void (*dp_mesh_diag_handler_t)(const berth_diag_t *d, uint16_t src_addr);

// Handlers are passed in at init time so they're stored before any
// rx path can fire. Sensor role ignores both fields.
typedef struct {
    dp_mesh_role_t role;
    dp_mesh_status_handler_t status_cb;
    dp_mesh_diag_handler_t diag_cb;
} dp_mesh_cfg_t;

esp_err_t dp_mesh_init(const dp_mesh_cfg_t *cfg);
esp_err_t dp_mesh_publish_status(const berth_status_t *s);
esp_err_t dp_mesh_publish_diag(const berth_diag_t *d);

#ifdef __cplusplus
}
#endif
