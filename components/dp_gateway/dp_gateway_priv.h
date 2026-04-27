#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dp_gateway_wifi_start_and_wait(void);

esp_err_t dp_gateway_mqtt_start_and_wait(void);
esp_err_t dp_gateway_mqtt_publish(const char *topic, const char *payload, int qos);

#ifdef __cplusplus
}
#endif

#endif
