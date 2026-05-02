#include "dp_gateway.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_gateway_priv.h"
#include "dp_prov.h"

static const char *TAG = "dp_gateway";

// Format the gateway's current wallclock as ISO 8601 UTC. If the system
// clock has not been set yet (no SNTP), this returns a 1970-epoch
// stamp — that's intentional: the backend is the source of truth on
// timestamps and can detect the unset-clock case
static void now_iso8601(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

// build berth_id from src_addr. dp_prov lookup first (adoption flow)
// else BERTH_ID_FORMAT + suffix table (bench fallback)
static const char *const SUFFIXES[12] = {
    "t1", "t2", "t3", "t4", "l1", "l2", "l3", "l4", "r1", "r2", "r3", "r4",
};

static bool render_berth_id(uint16_t src_addr, uint16_t fallback_idx, char *out, size_t cap)
{
    const char *mapped = dp_prov_lookup_berth(src_addr);
    if (mapped && *mapped) {
        strncpy(out, mapped, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    if (fallback_idx >= 1 && fallback_idx <= 12) {
        snprintf(out, cap, CONFIG_DOCKPULSE_BERTH_ID_FORMAT, SUFFIXES[fallback_idx - 1]);
        return true;
    }
    return false;
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
    dp_prov_init();
    esp_err_t err = dp_gateway_wifi_start_and_wait();
    if (err != ESP_OK) {
        return err;
    }
    err = dp_gateway_mqtt_start_and_wait();
    if (err != ESP_OK) {
        return err;
    }
    err = dp_gateway_adopt_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adopt init err=%d (provisioning won't work)", err);
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
    char berth_id[64];
    char ts_iso[32];
    if (!render_berth_id(src_addr, s->berth_id, berth_id, sizeof(berth_id))) {
        ESP_LOGW(TAG, "drop status, no berth mapping src=0x%04x payload_idx=%u", src_addr,
                 s->berth_id);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(node_id, sizeof(node_id), "node-%03u", s->node_id);
    now_iso8601(ts_iso, sizeof(ts_iso));

    // Topic format per docs/api/mqtt-contract.yml in the dockpulse repo:
    //   harbor/{harbor_id}/{dock_id}/{berth_id}/status
    // Backend's _parse_berth_topic (backend/app/mqtt.py) requires exactly
    // 5 path segments starting with "harbor/" or it silently drops the
    // message
    char topic[192];
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
    } else {
        ESP_LOGI(TAG, "publish topic=%s payload=%s", topic, payload);
    }
    cJSON_free(payload);
    return err;
#endif
}

esp_err_t dp_gateway_uplink_diag(const berth_diag_t *d, uint16_t src_addr)
{
    if (!d)
        return ESP_ERR_INVALID_ARG;
#if CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB
    ESP_LOGI(TAG, "uplink-stub diag src=0x%04x node=%u berth=%u target_state=%d raw_cm=%u",
             src_addr, d->node_id, d->berth_id, d->target_state, d->raw_distance_cm);
    return ESP_OK;
#else
    char node_id[16];
    char berth_id[64];
    char ts_iso[32];
    if (!render_berth_id(src_addr, d->berth_id, berth_id, sizeof(berth_id))) {
        ESP_LOGW(TAG, "drop diag, no berth mapping src=0x%04x payload_idx=%u", src_addr,
                 d->berth_id);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(node_id, sizeof(node_id), "node-%03u", d->node_id);
    now_iso8601(ts_iso, sizeof(ts_iso));

    char topic[192];
    snprintf(topic, sizeof(topic), "harbor/%s/%s/%s/diag", CONFIG_DOCKPULSE_HARBOR_ID,
             CONFIG_DOCKPULSE_DOCK_ID, berth_id);

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "node_id", node_id);
    cJSON_AddStringToObject(root, "berth_id", berth_id);
    cJSON_AddNumberToObject(root, "target_state", d->target_state);
    cJSON_AddNumberToObject(root, "raw_distance_cm", d->raw_distance_cm);

    cJSON *gates = cJSON_CreateArray();
    if (!gates) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < DP_RADAR_GATE_COUNT; i++) {
        cJSON_AddItemToArray(gates, cJSON_CreateNumber(d->gate_energy[i]));
    }
    cJSON_AddItemToObject(root, "gate_energy", gates);
    cJSON_AddStringToObject(root, "timestamp", ts_iso);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return ESP_ERR_NO_MEM;

    esp_err_t err = dp_gateway_mqtt_publish(topic, payload, CONFIG_DOCKPULSE_MQTT_QOS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish diag failed topic=%s err=%d", topic, err);
    } else {
        ESP_LOGI(TAG, "publish diag topic=%s len=%u", topic, (unsigned)strlen(payload));
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
esp_err_t dp_gateway_uplink_diag(const berth_diag_t *d, uint16_t src_addr)
{
    (void)d;
    (void)src_addr;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
