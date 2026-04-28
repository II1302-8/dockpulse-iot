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
// useful for diagnostics; the logical berth id lives in `s->berth_id`.
typedef void (*dp_mesh_status_handler_t)(const berth_status_t *s, uint16_t src_addr);

esp_err_t dp_mesh_init(dp_mesh_role_t role);
esp_err_t dp_mesh_publish_status(const berth_status_t *s);
esp_err_t dp_mesh_set_status_handler(dp_mesh_status_handler_t cb);

#ifdef __cplusplus
}
#endif
