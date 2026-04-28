#include "dp_gateway.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY

#include <stdio.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_gateway_priv.h"

static const char *TAG = "dp_gateway";

// Format the gateway's current wallclock as ISO 8601 UTC. If the system
// clock has not been set yet (no SNTP), this returns a 1970-epoch
// stamp — that's intentional: the backend is the source of truth on
// timestamps and can detect the unset-clock case.
static void now_iso8601(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

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
    berth_status_t fake = {
        .node_id = (uint8_t)CONFIG_DOCKPULSE_NODE_ID,
        .berth_id = (uint16_t)CONFIG_DOCKPULSE_NODE_ID,
        .occupied = true,
        .sensor_raw_mm = 1230,
        .battery_pct = DP_BATTERY_UNKNOWN,
        .ts_ms = 0,
    };
    dp_gateway_uplink(&fake, (uint16_t)CONFIG_DOCKPULSE_NODE_ID);
#endif
    return ESP_OK;
#endif
}

esp_err_t dp_gateway_uplink(const berth_status_t *s, uint16_t src_addr)
{
    if (!s)
        return ESP_ERR_INVALID_ARG;
#if CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB
    ESP_LOGI(TAG, "uplink-stub src=0x%04x node=%u berth=%u occupied=%d raw_mm=%u ts=%lu", src_addr,
             s->node_id, s->berth_id, s->occupied, s->sensor_raw_mm, (unsigned long)s->ts_ms);
    return ESP_OK;
#else
    char node_id[16];
    char berth_id[16];
    char ts_iso[32];
    snprintf(node_id, sizeof(node_id), "node-%03u", s->node_id);
    snprintf(berth_id, sizeof(berth_id), "berth-%03u", s->berth_id);
    now_iso8601(ts_iso, sizeof(ts_iso));

    // Topic format per docs/api/mqtt-contract.yml in the dockpulse repo:
    //   harbor/{harbor_id}/{dock_id}/{berth_id}/status
    // Backend's _parse_berth_topic (backend/app/mqtt.py) requires exactly
    // 5 path segments starting with "harbor/" or it silently drops the
    // message.
    char topic[96];
    snprintf(topic, sizeof(topic), "harbor/%s/%s/%s/status", CONFIG_DOCKPULSE_HARBOR_ID,
             CONFIG_DOCKPULSE_DOCK_ID, berth_id);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "node_id", node_id);
    cJSON_AddStringToObject(root, "berth_id", berth_id);
    cJSON_AddBoolToObject(root, "occupied", s->occupied);
    cJSON_AddNumberToObject(root, "sensor_raw", s->sensor_raw_mm);
    if (s->battery_pct != DP_BATTERY_UNKNOWN) {
        cJSON_AddNumberToObject(root, "battery_pct", s->battery_pct);
    }
    cJSON_AddStringToObject(root, "timestamp", ts_iso);

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
esp_err_t dp_gateway_uplink(const berth_status_t *s, uint16_t src_addr)
{
    (void)s;
    (void)src_addr;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
