#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DP_LED_OFF,
    DP_LED_OK,           // solid: provisioned + uplink OK
    DP_LED_IDLE,         // slow heartbeat: provisioned, no recent uplink
    DP_LED_PROVISIONING, // fast blink: PB-ADV unprovisioned beacon
    DP_LED_ERROR,        // double flash: disconnected
} dp_led_state_t;

esp_err_t dp_led_init(void);
void dp_led_set(dp_led_state_t state);

// dual-color berth-status LED (red/green common-cathode), separate from
// the system status LED above. green = free, red = occupied. no-op if
// both gpios are -1.
esp_err_t dp_berth_led_init(void);
void dp_berth_led_set(bool occupied);

// Long-press cb runs in dedicated task. Typical impl: dp_prov_factory_reset().
typedef void (*dp_button_long_press_cb_t)(void *user_ctx);
esp_err_t dp_button_init(dp_button_long_press_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
