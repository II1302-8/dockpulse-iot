#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_mesh.h"
#include "dp_prov.h"

static const char *TAG = "dp_gw_decom";

static void publish_resp(const char *req_id, const char *node_id, const char *status,
                         const char *code, const char *msg)
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
        publish_resp(req_id, node_id, "err", "bad-req", "missing unicast_addr");
        cJSON_Delete(root);
        return;
    }

    // strtoul accepts 0xNNNN or NNNN, base 0 sniffs prefix
    char *end = NULL;
    unsigned long parsed = strtoul(unicast_str, &end, 0);
    if (end == unicast_str || parsed == 0 || parsed > 0xFFFF) {
        ESP_LOGW(TAG, "bad unicast_addr=%s req=%s", unicast_str, req_id ? req_id : "?");
        publish_resp(req_id, node_id, "err", "bad-unicast", NULL);
        cJSON_Delete(root);
        return;
    }
    uint16_t addr = (uint16_t)parsed;

    // free the unicast slot in the mesh provisioner table else the addr
    // allocator keeps walking forward and we leak slots forever
    esp_err_t mesh_err = dp_mesh_gateway_delete_node(addr);
    if (mesh_err != ESP_OK && mesh_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "delete_node addr=0x%04x err=%d", addr, mesh_err);
    }

    esp_err_t err = dp_prov_forget_unicast(addr);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "decom req=%s node=%s addr=0x%04x", req_id ? req_id : "?",
                 node_id ? node_id : "?", addr);
        publish_resp(req_id, node_id, "ok", NULL, NULL);
    } else {
        ESP_LOGW(TAG, "forget addr=0x%04x err=%d", addr, err);
        publish_resp(req_id, node_id, "err", "nvs-write", NULL);
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
