#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dp_gateway_wifi_start_and_wait(void);

// MQTT message dispatch — set before start_and_wait so subscription
// callbacks have somewhere to land on first connect
typedef void (*dp_gateway_mqtt_msg_cb_t)(const char *topic, const char *payload, int payload_len);

esp_err_t dp_gateway_mqtt_set_msg_cb(dp_gateway_mqtt_msg_cb_t cb);
esp_err_t dp_gateway_mqtt_subscribe(const char *topic_filter, int qos);
esp_err_t dp_gateway_mqtt_start_and_wait(void);
esp_err_t dp_gateway_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t dp_gateway_mqtt_publish_retained(const char *topic, const char *payload, int qos);

// Adoption: subscribes to dockpulse/v1/gw/{gw_id}/provision/req, runs
// PB-ADV via dp_mesh, replies on dockpulse/v1/gw/{gw_id}/provision/resp,
// publishes retained {online:true} on dockpulse/v1/gw/{gw_id}/status
// (LWT publishes {online:false} on disconnect)
esp_err_t dp_gateway_adopt_init(void);

#ifdef __cplusplus
}
#endif

#endif
