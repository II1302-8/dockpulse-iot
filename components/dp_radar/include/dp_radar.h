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

// Re-send the Report-mode command. Call this from a recovery path
// (e.g., after N consecutive read timeouts) to nudge a module that
// has dropped out of streaming.
esp_err_t dp_radar_enter_report_mode(void);

#ifdef __cplusplus
}
#endif
