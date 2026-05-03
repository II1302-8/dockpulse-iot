#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "dp_mesh.h"
#include "dp_prov.h"

static const char *TAG = "dp_gw_adopt";

#define DEFAULT_TTL_MS 60000

// stack handles one provision flow at a time
static struct {
    bool active;
    char req_id[64];
    char berth_id[DP_PROV_BERTH_ID_MAX];
} s_inflight;

static void publish_resp_ok(const char *req_id, uint16_t addr, const uint8_t dev_key[16])
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "req_id", req_id);
    cJSON_AddStringToObject(root, "status", "ok");
    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "0x%04x", addr);
    cJSON_AddStringToObject(root, "unicast_addr", addr_str);
    // dev_key fingerprint = first 8 bytes sha256(dev_key) hex-encoded
    uint8_t hash[32];
    mbedtls_sha256(dev_key, 16, hash, 0);
    char fp[17];
    for (int i = 0; i < 8; i++) {
        snprintf(fp + i * 2, 3, "%02x", hash[i]);
    }
    fp[16] = '\0';
    cJSON_AddStringToObject(root, "dev_key_fp", fp);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "dockpulse/v1/gw/%s/provision/resp",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    dp_gateway_mqtt_publish(topic, payload, 1);
    ESP_LOGI(TAG, "resp ok req=%s addr=%s fp=%s", req_id, addr_str, fp);
    cJSON_free(payload);
}

static void publish_resp_err(const char *req_id, const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "req_id", req_id);
    cJSON_AddStringToObject(root, "status", "err");
    cJSON_AddStringToObject(root, "code", code ? code : "unknown");
    if (msg) {
        cJSON_AddStringToObject(root, "msg", msg);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "dockpulse/v1/gw/%s/provision/resp",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    dp_gateway_mqtt_publish(topic, payload, 1);
    ESP_LOGW(TAG, "resp err req=%s code=%s msg=%s", req_id, code ? code : "?", msg ? msg : "");
    cJSON_free(payload);
}

static void on_prov_done(const dp_mesh_prov_result_t *res, void *ctx)
{
    (void)ctx;
    if (!s_inflight.active) {
        return;
    }
    if (res->ok) {
        // record berth mapping if backend sent one. else uplink falls
        // back to addr-as-berth (see dp_gateway_uplink)
        if (s_inflight.berth_id[0]) {
            dp_prov_record_berth(res->unicast_addr, s_inflight.berth_id);
        }
        publish_resp_ok(s_inflight.req_id, res->unicast_addr, res->dev_key);
    } else {
        publish_resp_err(s_inflight.req_id, res->err_code, res->err_msg);
    }
    s_inflight.active = false;
    s_inflight.req_id[0] = '\0';
    s_inflight.berth_id[0] = '\0';
}

// hex string -> bytes; returns ESP_OK iff exactly 2*expected_bytes hex chars
static esp_err_t hex_decode(const char *hex, uint8_t *out, size_t expected_bytes)
{
    size_t hlen = strlen(hex);
    if (hlen != expected_bytes * 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < expected_bytes; i++) {
        unsigned int v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)v;
    }
    return ESP_OK;
}

static void handle_provision_req(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "bad json on provision/req");
        return;
    }
    const char *req_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "req_id"));
    const char *uuid_hex = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "uuid"));
    const char *oob_hex = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "oob"));
    cJSON *ttl_j = cJSON_GetObjectItemCaseSensitive(root, "ttl_s");
    // optional. backend may pass assigned berth
    const char *berth = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "berth_id"));

    if (!req_id || !uuid_hex) {
        ESP_LOGW(TAG, "missing req_id/uuid");
        cJSON_Delete(root);
        return;
    }
    if (s_inflight.active) {
        publish_resp_err(req_id, "busy", NULL);
        cJSON_Delete(root);
        return;
    }
    uint8_t uuid[16];
    if (hex_decode(uuid_hex, uuid, 16) != ESP_OK) {
        publish_resp_err(req_id, "bad-uuid", NULL);
        cJSON_Delete(root);
        return;
    }
    uint8_t oob[16] = {0};
    bool have_oob = false;
    if (oob_hex && *oob_hex) {
        if (hex_decode(oob_hex, oob, 16) != ESP_OK) {
            publish_resp_err(req_id, "bad-oob", NULL);
            cJSON_Delete(root);
            return;
        }
        have_oob = true;
    }
    uint32_t timeout_ms = DEFAULT_TTL_MS;
    if (cJSON_IsNumber(ttl_j) && ttl_j->valueint > 0) {
        timeout_ms = (uint32_t)ttl_j->valueint * 1000U;
    }

    s_inflight.active = true;
    strncpy(s_inflight.req_id, req_id, sizeof(s_inflight.req_id) - 1);
    s_inflight.req_id[sizeof(s_inflight.req_id) - 1] = '\0';
    s_inflight.berth_id[0] = '\0';
    if (berth && *berth) {
        strncpy(s_inflight.berth_id, berth, sizeof(s_inflight.berth_id) - 1);
        s_inflight.berth_id[sizeof(s_inflight.berth_id) - 1] = '\0';
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "req %s uuid=%02x%02x%02x... oob=%d berth=%s", s_inflight.req_id, uuid[0],
             uuid[1], uuid[2], have_oob ? 1 : 0, s_inflight.berth_id);
    esp_err_t err =
        dp_mesh_gateway_provision(uuid, have_oob ? oob : NULL, timeout_ms, on_prov_done, NULL);
    if (err != ESP_OK) {
        publish_resp_err(s_inflight.req_id, "start-fail", esp_err_to_name(err));
        s_inflight.active = false;
        s_inflight.req_id[0] = '\0';
    }
}

static void on_msg(const char *topic, const char *payload, int len)
{
    char expect[160];
    snprintf(expect, sizeof(expect), "dockpulse/v1/gw/%s/provision/req",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    if (strcmp(topic, expect) == 0) {
        handle_provision_req(payload, len);
    }
}

esp_err_t dp_gateway_adopt_init(void)
{
    esp_err_t err = dp_gateway_mqtt_add_msg_cb(on_msg);
    if (err != ESP_OK) {
        return err;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "dockpulse/v1/gw/%s/provision/req", CONFIG_DOCKPULSE_GATEWAY_ID);
    return dp_gateway_mqtt_subscribe(topic, 1);
}

#endif
