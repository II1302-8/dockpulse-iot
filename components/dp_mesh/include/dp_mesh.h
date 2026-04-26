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

typedef void (*dp_mesh_sample_handler_t)(const dp_radar_sample_t *s, uint16_t src_addr);

esp_err_t dp_mesh_init(dp_mesh_role_t role);
esp_err_t dp_mesh_publish_sample(const dp_radar_sample_t *s);
esp_err_t dp_mesh_set_sample_handler(dp_mesh_sample_handler_t cb);

#ifdef __cplusplus
}
#endif
