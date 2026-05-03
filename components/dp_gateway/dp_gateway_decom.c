#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "dp_prov.h"

static const char *TAG = "dp_gw_decom";

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
        cJSON_Delete(root);
        return;
    }

    // strtoul accepts 0xNNNN or NNNN, base 0 sniffs prefix
    char *end = NULL;
    unsigned long parsed = strtoul(unicast_str, &end, 0);
    if (end == unicast_str || parsed == 0 || parsed > 0xFFFF) {
        ESP_LOGW(TAG, "bad unicast_addr=%s req=%s", unicast_str, req_id ? req_id : "?");
        cJSON_Delete(root);
        return;
    }
    uint16_t addr = (uint16_t)parsed;

    esp_err_t err = dp_prov_forget_unicast(addr);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "decom req=%s node=%s addr=0x%04x", req_id ? req_id : "?",
                 node_id ? node_id : "?", addr);
    } else {
        ESP_LOGW(TAG, "forget addr=0x%04x err=%d", addr, err);
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
