#include "dp_gateway.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY

#include "esp_log.h"

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
    // TODO: bring up Wi-Fi / Ethernet / cellular and the chosen uplink
    // (MQTT to backend, HTTP, etc.) when uplink_stub is disabled.
    return ESP_OK;
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
    (void)src_addr;
    return ESP_ERR_NOT_SUPPORTED; // wire MQTT/HTTP here
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
