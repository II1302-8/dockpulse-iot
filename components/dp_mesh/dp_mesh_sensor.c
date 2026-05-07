// NODE side. cfg-server rx provisioner pushes. vnd-model tx

#include <inttypes.h>

#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dp_mesh_priv.h"
#include "dp_prov.h"

static const char *TAG = "dp_mesh_sensor";

static dp_mesh_status_handler_t s_status_cb;
static dp_mesh_diag_handler_t s_diag_cb;
static dp_mesh_sensor_ready_cb_t s_ready_cb;
static bool s_ready_fired;

// armed at PB-ADV done, disarmed at PubSet, expiry means cfg never finished
// so factory-reset avoids sitting half-configured forever
#define POST_PROV_WATCHDOG_US (90ULL * 1000 * 1000)
static esp_timer_handle_t s_post_prov_timer;

static void post_prov_timer_cb(void *arg)
{
    (void)arg;
    if (s_ready_fired) {
        return;
    }
    ESP_LOGW(TAG, "post-prov watchdog: cfg never completed, resetting node");
    esp_err_t err = esp_ble_mesh_node_local_reset();
    if (err != ESP_OK) {
        // last resort if local_reset failed
        ESP_LOGW(TAG, "node_local_reset err=%d, factory-reset", err);
        dp_prov_factory_reset();
    }
}

static void arm_post_prov_watchdog(void)
{
    if (!s_post_prov_timer) {
        const esp_timer_create_args_t a = {
            .callback = post_prov_timer_cb,
            .name = "dp_post_prov_to",
        };
        if (esp_timer_create(&a, &s_post_prov_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_post_prov_timer);
    esp_timer_start_once(s_post_prov_timer, POST_PROV_WATCHDOG_US);
}

static void disarm_post_prov_watchdog(void)
{
    if (s_post_prov_timer) {
        esp_timer_stop(s_post_prov_timer);
    }
}

void dp_mesh_sensor_set_callbacks(dp_mesh_status_handler_t status_cb,
                                  dp_mesh_diag_handler_t diag_cb,
                                  dp_mesh_sensor_ready_cb_t ready_cb)
{
    s_status_cb = status_cb;
    s_diag_cb = diag_cb;
    s_ready_cb = ready_cb;
}

void dp_mesh_sensor_fire_ready(void)
{
    if (s_ready_fired) {
        return;
    }
    uint16_t addr = esp_ble_mesh_get_primary_element_address();
    dp_mesh_internal_arm_sensor_pub();
    s_ready_fired = true;
    disarm_post_prov_watchdog();
    ESP_LOGI(TAG, "sensor ready addr=0x%04x", addr);
    if (s_ready_cb) {
        s_ready_cb(addr);
    }
}

void dp_mesh_sensor_handle_prov_event(esp_ble_mesh_prov_cb_event_t event,
                                      esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "node beacon on err=%d", param->node_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "node prov link open bearer=%d", param->node_prov_link_open.bearer);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "node prov link close bearer=%d reason=%d",
                 param->node_prov_link_close.bearer, param->node_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "node prov complete addr=0x%04x net_idx=0x%04x iv=0x%08" PRIx32,
                 param->node_prov_complete.addr, param->node_prov_complete.net_idx,
                 param->node_prov_complete.iv_index);
        // wait for AppKey Add + Model App Bind + Pub Set via cfg server below
        // watchdog catches interrupted cfg phases that would leave us half-set
        arm_post_prov_watchdog();
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGW(TAG, "node prov reset, factory wiping");
        // no-return on success
        dp_prov_factory_reset();
        break;
    default:
        ESP_LOGD(TAG, "node prov evt %d", (int)event);
        break;
    }
}

// fires when provisioner pushes AppKey Add / Model App Bind / Model Pub Set
void dp_mesh_sensor_on_cfg_server(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        return;
    }
    uint32_t op = param->ctx.recv_op;
    ESP_LOGI(TAG, "cfg srv state-change op=0x%04" PRIx32, op);
    // PubSet only, Bind alone leaves publish_addr unset so model can't
    // publish to the group, firing on bind looked ready but dropped traffic
    if (op == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        dp_mesh_sensor_fire_ready();
    }
}

esp_err_t dp_mesh_publish_status(const berth_status_t *s)
{
    if (!s || dp_mesh_get_role() != DP_MESH_ROLE_SENSOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready_fired || !dp_mesh_internal_sensor_pub_armed()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t wire[BERTH_STATUS_WIRE_LEN];
    size_t wire_len = 0;
    esp_err_t err = berth_status_pack(s, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }
    err = dp_mesh_internal_publish(DP_OP_STATUS_PUB, wire, (uint16_t)wire_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish err=%d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t dp_mesh_publish_diag(const berth_diag_t *d)
{
    if (!d || dp_mesh_get_role() != DP_MESH_ROLE_SENSOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready_fired || !dp_mesh_internal_sensor_pub_armed()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t wire[BERTH_DIAG_WIRE_LEN];
    size_t wire_len = 0;
    esp_err_t err = berth_diag_pack(d, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }
    err = dp_mesh_internal_publish(DP_OP_DIAG_PUB, wire, (uint16_t)wire_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish diag err=%d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}
