#pragma once

#include "dp_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dp_radar_init(void);
esp_err_t dp_radar_read(dp_radar_sample_t *out, TickType_t timeout);
esp_err_t dp_radar_deinit(void);

#ifdef __cplusplus
}
#endif
