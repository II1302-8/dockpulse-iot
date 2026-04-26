#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool presence;
    uint16_t distance_cm;
    int8_t target_state; // raw radar state byte; sensor-specific meaning
    uint32_t ts_ms;      // monotonic ms at sample time
} dp_radar_sample_t;

esp_err_t dp_common_get_node_id(uint8_t *out);

#ifdef __cplusplus
}
#endif
