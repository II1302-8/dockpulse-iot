#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_mesh.h"

static const char *TAG = "dp_gw_decom";

static void publish_resp(const char *req_id, const char *node_id, const char *status,
                         const char *code, const char *msg, int attempts)
{
    if (!req_id) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "req_id", req_id);
    if (node_id) {
        cJSON_AddStringToObject(root, "node_id", node_id);
    }
    cJSON_AddStringToObject(root, "status", status);
    if (code) {
        cJSON_AddStringToObject(root, "code", code);
    }
    if (msg) {
        cJSON_AddStringToObject(root, "msg", msg);
    }
    if (attempts > 0) {
        cJSON_AddNumberToObject(root, "attempts", attempts);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "dockpulse/v1/gw/%s/decommission/resp",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    dp_gateway_mqtt_publish(topic, payload, 1);
    cJSON_free(payload);
}

// owned by req handler, freed in the decom callback. inline storage keeps
// the heap quiet under bursts of decoms
typedef struct {
    char req_id[64];
    char node_id[64];
} decom_ctx_t;

static void on_decom_done(dp_mesh_decom_result_t result, int attempts,
                          const char *err_code, void *ctx)
{
    decom_ctx_t *c = (decom_ctx_t *)ctx;
    const char *status = result == DP_MESH_DECOM_OK     ? "ok"
                         : result == DP_MESH_DECOM_ORPHAN ? "orphan"
                                                          : "err";
    ESP_LOGI(TAG, "decom done req=%s status=%s attempts=%d", c->req_id, status, attempts);
    publish_resp(c->req_id, c->node_id[0] ? c->node_id : NULL, status, err_code, NULL,
                 attempts);
    free(c);
}

static void handle_decommission_req(const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "bad json on decommission/req");
        return;
    }
    const char *req_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "req_id"));
    const char *unicast_str =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "unicast_addr"));
    const char *node_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "node_id"));

    if (!unicast_str) {
        ESP_LOGW(TAG, "missing unicast_addr req=%s", req_id ? req_id : "?");
        publish_resp(req_id, node_id, "err", "bad-req", "missing unicast_addr", 0);
        cJSON_Delete(root);
        return;
    }

    // strtoul accepts 0xNNNN or NNNN, base 0 sniffs prefix
    char *end = NULL;
    unsigned long parsed = strtoul(unicast_str, &end, 0);
    if (end == unicast_str || parsed == 0 || parsed > 0xFFFF) {
        ESP_LOGW(TAG, "bad unicast_addr=%s req=%s", unicast_str, req_id ? req_id : "?");
        publish_resp(req_id, node_id, "err", "bad-unicast", NULL, 0);
        cJSON_Delete(root);
        return;
    }
    uint16_t addr = (uint16_t)parsed;

    decom_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) {
        publish_resp(req_id, node_id, "err", "unknown", "oom", 0);
        cJSON_Delete(root);
        return;
    }
    if (req_id) {
        strncpy(c->req_id, req_id, sizeof(c->req_id) - 1);
    }
    if (node_id) {
        strncpy(c->node_id, node_id, sizeof(c->node_id) - 1);
    }

    esp_err_t err = dp_mesh_gateway_decommission_node(addr, on_decom_done, c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decom_node start err=%d addr=0x%04x", err, addr);
        const char *msg = err == ESP_ERR_INVALID_STATE
                              ? "another decommission in flight"
                              : NULL;
        publish_resp(req_id, node_id, "err", "unknown", msg, 0);
        free(c);
    }
    cJSON_Delete(root);
}

static void on_msg(const char *topic, const char *payload, int len)
{
    char expect[160];
    snprintf(expect, sizeof(expect), "dockpulse/v1/gw/%s/decommission/req",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    if (strcmp(topic, expect) == 0) {
        handle_decommission_req(payload, len);
    }
}

esp_err_t dp_gateway_decom_init(void)
{
    esp_err_t err = dp_gateway_mqtt_add_msg_cb(on_msg);
    if (err != ESP_OK) {
        return err;
    }
    char topic[160];
    snprintf(topic, sizeof(topic), "dockpulse/v1/gw/%s/decommission/req",
             CONFIG_DOCKPULSE_GATEWAY_ID);
    return dp_gateway_mqtt_subscribe(topic, 1);
}

#endif
