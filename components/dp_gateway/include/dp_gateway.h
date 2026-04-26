#pragma once

#include "dp_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dp_gateway_init(void);
esp_err_t dp_gateway_uplink(const dp_radar_sample_t *s, uint16_t src_addr);

#ifdef __cplusplus
}
#endif
