#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dp_gateway_wifi_start_and_wait(void);

// register before start_and_wait so first-connect subs have a landing pad
// rx fans out to all callbacks, each does its own topic filtering
typedef void (*dp_gateway_mqtt_msg_cb_t)(const char *topic, const char *payload, int payload_len);

esp_err_t dp_gateway_mqtt_add_msg_cb(dp_gateway_mqtt_msg_cb_t cb);
esp_err_t dp_gateway_mqtt_subscribe(const char *topic_filter, int qos);
esp_err_t dp_gateway_mqtt_start_and_wait(void);
esp_err_t dp_gateway_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t dp_gateway_mqtt_publish_retained(const char *topic, const char *payload, int qos);

// adoption, subs provision/req runs PB-ADV via dp_mesh replies provision/resp
// retained {online:true} on /status, LWT {online:false} on disconnect
esp_err_t dp_gateway_adopt_init(void);

// decommission, subs decommission/req drops the unicast→berth mapping
esp_err_t dp_gateway_decom_init(void);

#ifdef __cplusplus
}
#endif

#endif
