#include "dp_gateway.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_gateway_priv.h"

static const char *TAG = "dp_gateway";

esp_err_t dp_gateway_init(void)
{
    ESP_LOGI(TAG, "init (uplink_stub=%d)",
#if CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB
             1
#else
             0
#endif
    );

#if CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB
    return ESP_OK;
#else
    esp_err_t err = dp_gateway_wifi_start_and_wait();
    if (err != ESP_OK) {
        return err;
    }
    err = dp_gateway_mqtt_start_and_wait();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_DOCKPULSE_MQTT_SELFTEST
    dp_radar_sample_t fake = {.presence = true, .distance_cm = 123, .target_state = 1, .ts_ms = 0};
    dp_gateway_uplink(&fake, (uint16_t)CONFIG_DOCKPULSE_NODE_ID);
#endif
    return ESP_OK;
#endif
}

esp_err_t dp_gateway_uplink(const dp_radar_sample_t *s, uint16_t src_addr)
{
    if (!s)
        return ESP_ERR_INVALID_ARG;
#if CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB
    ESP_LOGI(TAG, "uplink-stub src=0x%04x presence=%d distance_cm=%u ts=%lu", src_addr, s->presence,
             s->distance_cm, (unsigned long)s->ts_ms);
    return ESP_OK;
#else
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%04x", CONFIG_DOCKPULSE_MQTT_TOPIC_BASE, src_addr);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "berth_id", src_addr);
    cJSON_AddBoolToObject(root, "presence", s->presence);
    cJSON_AddNumberToObject(root, "distance_cm", s->distance_cm);
    cJSON_AddNumberToObject(root, "target_state", s->target_state);
    cJSON_AddNumberToObject(root, "ts_ms", s->ts_ms);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return ESP_ERR_NO_MEM;

    esp_err_t err = dp_gateway_mqtt_publish(topic, payload, CONFIG_DOCKPULSE_MQTT_QOS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish failed topic=%s err=%d", topic, err);
    }
    cJSON_free(payload);
    return err;
#endif
}

#else // !CONFIG_DOCKPULSE_ROLE_GATEWAY — stub out for sensor build

esp_err_t dp_gateway_init(void) { return ESP_OK; }
esp_err_t dp_gateway_uplink(const dp_radar_sample_t *s, uint16_t src_addr)
{
    (void)s;
    (void)src_addr;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
