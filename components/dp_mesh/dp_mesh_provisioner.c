// GATEWAY side. PB-ADV by uuid-match then AppKeyAdd / Bind / PubSet via cfg-client

#include <inttypes.h>
#include <string.h>

#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dp_mesh_priv.h"
#include "dp_prov.h"

static const char *TAG = "dp_mesh_prov";

static dp_mesh_status_handler_t s_status_cb;
static dp_mesh_diag_handler_t s_diag_cb;

// in-flight provisioning state
static dp_mesh_prov_done_cb_t s_prov_cb;
static void *s_prov_ctx;
static uint8_t s_prov_uuid[16];
static bool s_prov_in_flight;
static uint16_t s_prov_target_addr;
static uint8_t s_prov_dev_key[16];
static esp_timer_handle_t s_prov_timer;

// state callback fires at phase transitions so the adopt layer can mirror
// progress on MQTT. always called from mesh-stack threads
static dp_mesh_prov_state_cb_t s_state_cb;
static void *s_state_ctx;

static void emit_state(const char *state)
{
    if (s_state_cb && s_prov_in_flight) {
        s_state_cb(state, s_state_ctx);
    }
}

void dp_mesh_gateway_set_state_cb(dp_mesh_prov_state_cb_t cb, void *ctx)
{
    s_state_cb = cb;
    s_state_ctx = ctx;
}

// cfg client step machine for post-PB-ADV configuration
typedef enum {
    CFG_STEP_IDLE = 0,
    CFG_STEP_APP_KEY_ADD,
    CFG_STEP_MODEL_APP_BIND,
    CFG_STEP_MODEL_PUB_SET,
    CFG_STEP_DONE,
} cfg_step_t;
static cfg_step_t s_cfg_step;

// 1 initial + 2 retries per step. transient cfg-phase packet loss had
// been dooming adoptions where the node would have reapplied on retry
#define CFG_MAX_ATTEMPTS 3
static int s_cfg_attempts;

static void cfg_step_advance(uint16_t addr);
static void cfg_step_redo(uint16_t addr);
static esp_err_t cfg_do_current_step(uint16_t addr);
static void prov_finish_ok(uint16_t addr, const uint8_t dev_key[16]);
static void prov_finish_err(const char *code, const char *msg);

// disarm auto-prov so stale uuid match from previous flow can't fire on
// a re-beacon outside an active request. zero-len match + prov_after_match=false
// matches everything but never auto-provisions
static void clear_dev_uuid_match(void)
{
    esp_err_t err = esp_ble_mesh_provisioner_set_dev_uuid_match(NULL, 0, 0, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear uuid match err=%d", err);
    }
}

void dp_mesh_provisioner_set_callbacks(dp_mesh_status_handler_t status_cb,
                                       dp_mesh_diag_handler_t diag_cb)
{
    s_status_cb = status_cb;
    s_diag_cb = diag_cb;
}

// ----- cfg-client cb. fires after each AppKeyAdd/Bind/PubSet response -----

static void on_cfg_client(esp_ble_mesh_cfg_client_cb_event_t event,
                          esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!s_prov_in_flight) {
        return;
    }
    int err = param->error_code;
    uint32_t op = param->params ? param->params->opcode : 0;
    ESP_LOGI(TAG, "cfg cli evt=%d op=0x%04" PRIx32 " err=%d step=%d att=%d", (int)event, op, err,
             (int)s_cfg_step, s_cfg_attempts);
    if (event == ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT) {
        // not a response to our cmd
        return;
    }
    if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        // transient loss retry, err!=0 path below stays terminal
        if (s_cfg_attempts < CFG_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "cfg timeout step=%d att=%d, retry", (int)s_cfg_step, s_cfg_attempts);
            cfg_step_redo(s_prov_target_addr);
            return;
        }
        prov_finish_err("cfg-fail", NULL);
        return;
    }
    if (err != 0) {
        // semantic error from node, not transient
        prov_finish_err("cfg-fail", NULL);
        return;
    }
    cfg_step_advance(s_prov_target_addr);
}

esp_err_t dp_mesh_provisioner_register(void)
{
    return esp_ble_mesh_register_config_client_callback(on_cfg_client);
}

// ----- prov events. dispatched from on_prov when role==GATEWAY -----

void dp_mesh_provisioner_handle_prov_event(esp_ble_mesh_prov_cb_event_t event,
                                           esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "prov enable err=%d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "netkey added err=%d", param->provisioner_add_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "netkey updated err=%d", param->provisioner_update_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "appkey added err=%d", param->provisioner_add_app_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "local bind err=%d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *u = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        ESP_LOGD(TAG, "unprov beacon uuid=%02x%02x%02x...", u[0], u[1], u[2]);
        // matching via set_dev_uuid_match auto-triggers provisioning,
        // nothing to do here besides log
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "prov link open bearer=%d", param->provisioner_prov_link_open.bearer);
        emit_state("link-open");
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGW(TAG, "prov link close bearer=%d reason=%d",
                 param->provisioner_prov_link_close.bearer,
                 param->provisioner_prov_link_close.reason);
        if (s_prov_in_flight && s_cfg_step < CFG_STEP_APP_KEY_ADD) {
            prov_finish_err("link-close", NULL);
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t addr = param->provisioner_prov_complete.unicast_addr;
        ESP_LOGI(TAG, "prov complete addr=0x%04x node_idx=%u", addr,
                 param->provisioner_prov_complete.node_idx);
        // orphan complete: stack adopted a beacon outside an active flow
        // (e.g. stale uuid match never cleared). delete it so it can't
        // sit on a unicast slot half-configured
        if (!s_prov_in_flight) {
            ESP_LOGW(TAG, "orphan prov complete addr=0x%04x, deleting", addr);
            esp_ble_mesh_provisioner_delete_node_with_addr(addr);
            break;
        }
        s_prov_target_addr = addr;
        const esp_ble_mesh_node_t *node = esp_ble_mesh_provisioner_get_node_with_addr(addr);
        if (node) {
            memcpy(s_prov_dev_key, node->dev_key, 16);
        } else {
            ESP_LOGW(TAG, "node lookup failed for addr=0x%04x", addr);
            memset(s_prov_dev_key, 0, 16);
        }
        emit_state("pb-adv-done");
        cfg_step_advance(addr);
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "uuid-match set err=%d", param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_STATIC_OOB_VALUE_COMP_EVT:
        ESP_LOGI(TAG, "static-oob set err=%d", param->provisioner_set_static_oob_val_comp.err_code);
        break;
    default:
        ESP_LOGD(TAG, "prov evt %d", (int)event);
        break;
    }
}

// ----- model-rx. dispatched from on_model OPERATION_EVT when role==GATEWAY -----

void dp_mesh_provisioner_handle_model_op(esp_ble_mesh_model_cb_param_t *param)
{
    if (param->model_operation.opcode == DP_OP_STATUS_PUB) {
        berth_status_t s;
        if (berth_status_unpack(param->model_operation.msg, param->model_operation.length, &s) !=
            ESP_OK) {
            ESP_LOGW(TAG, "rx status unpack fail len=%u", param->model_operation.length);
            return;
        }
        if (s_status_cb) {
            s_status_cb(&s, param->model_operation.ctx->addr);
        }
    } else if (param->model_operation.opcode == DP_OP_DIAG_PUB) {
        berth_diag_t d;
        if (berth_diag_unpack(param->model_operation.msg, param->model_operation.length, &d) !=
            ESP_OK) {
            ESP_LOGW(TAG, "rx diag unpack fail len=%u", param->model_operation.length);
            return;
        }
        if (s_diag_cb) {
            s_diag_cb(&d, param->model_operation.ctx->addr);
        }
    }
}

// ----- cfg-client send + step machine -----

static esp_err_t cfg_send(uint16_t addr, uint32_t opcode, esp_ble_mesh_cfg_client_set_state_t *set)
{
    // cfg cli is the second SIG model on our root element
    esp_ble_mesh_client_common_param_t common = {
        .opcode = opcode,
        .model = &root_models_gateway[1],
        .ctx =
            {
                .net_idx = DP_NET_IDX,
                .app_idx = ESP_BLE_MESH_KEY_UNUSED,
                .addr = addr,
                .send_ttl = 7,
            },
        // 8000ms tolerates slow first response after PB-ADV, retries gated separately
        .msg_timeout = 8000,
    };
    return esp_ble_mesh_config_client_set_state(&common, set);
}

// caller manages step counter + attempts, this only sends
static esp_err_t cfg_do_current_step(uint16_t addr)
{
    switch (s_cfg_step) {
    case CFG_STEP_APP_KEY_ADD: {
        esp_ble_mesh_cfg_client_set_state_t set = {0};
        set.app_key_add.net_idx = DP_NET_IDX;
        set.app_key_add.app_idx = DP_APP_IDX;
        memcpy(set.app_key_add.app_key, DP_APP_KEY, 16);
        return cfg_send(addr, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD, &set);
    }
    case CFG_STEP_MODEL_APP_BIND: {
        esp_ble_mesh_cfg_client_set_state_t set = {0};
        set.model_app_bind.element_addr = addr;
        set.model_app_bind.model_app_idx = DP_APP_IDX;
        set.model_app_bind.model_id = DP_VND_MODEL_ID;
        set.model_app_bind.company_id = DP_CID;
        return cfg_send(addr, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND, &set);
    }
    case CFG_STEP_MODEL_PUB_SET: {
        esp_ble_mesh_cfg_client_set_state_t set = {0};
        set.model_pub_set.element_addr = addr;
        set.model_pub_set.publish_addr = DP_GROUP_ADDR;
        set.model_pub_set.publish_app_idx = DP_APP_IDX;
        set.model_pub_set.publish_ttl = 7;
        set.model_pub_set.publish_period = 0;
        set.model_pub_set.publish_retransmit = ESP_BLE_MESH_PUBLISH_TRANSMIT(1, 50);
        set.model_pub_set.model_id = DP_VND_MODEL_ID;
        set.model_pub_set.company_id = DP_CID;
        return cfg_send(addr, ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET, &set);
    }
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

static void cfg_step_advance(uint16_t addr)
{
    s_cfg_step++;
    s_cfg_attempts = 1;
    switch (s_cfg_step) {
    case CFG_STEP_APP_KEY_ADD:
        emit_state("cfg-app-key");
        if (cfg_do_current_step(addr) != ESP_OK) {
            prov_finish_err("appkey-send", NULL);
        }
        break;
    case CFG_STEP_MODEL_APP_BIND:
        emit_state("cfg-bind");
        if (cfg_do_current_step(addr) != ESP_OK) {
            prov_finish_err("bind-send", NULL);
        }
        break;
    case CFG_STEP_MODEL_PUB_SET:
        emit_state("cfg-pub-set");
        if (cfg_do_current_step(addr) != ESP_OK) {
            prov_finish_err("pubset-send", NULL);
        }
        break;
    case CFG_STEP_DONE:
        prov_finish_ok(addr, s_prov_dev_key);
        break;
    default:
        break;
    }
}

// re-send current step without advancing s_cfg_step
static void cfg_step_redo(uint16_t addr)
{
    s_cfg_attempts++;
    if (cfg_do_current_step(addr) != ESP_OK) {
        // map send-side failure to per-step code so backend can disambiguate
        const char *code = "cfg-fail";
        switch (s_cfg_step) {
        case CFG_STEP_APP_KEY_ADD:
            code = "appkey-send";
            break;
        case CFG_STEP_MODEL_APP_BIND:
            code = "bind-send";
            break;
        case CFG_STEP_MODEL_PUB_SET:
            code = "pubset-send";
            break;
        default:
            break;
        }
        prov_finish_err(code, NULL);
    }
}

static void prov_timer_cb(void *arg)
{
    (void)arg;
    if (s_prov_in_flight) {
        ESP_LOGW(TAG, "prov timeout uuid=%02x%02x...", s_prov_uuid[0], s_prov_uuid[1]);
        prov_finish_err("timeout", NULL);
    }
}

static void prov_finish_ok(uint16_t addr, const uint8_t dev_key[16])
{
    if (!s_prov_in_flight) {
        return;
    }
    emit_state("complete");
    s_prov_in_flight = false;
    s_cfg_step = CFG_STEP_IDLE;
    if (s_prov_timer) {
        esp_timer_stop(s_prov_timer);
    }
    clear_dev_uuid_match();
    dp_mesh_prov_result_t res = {.ok = true, .unicast_addr = addr};
    memcpy(res.dev_key, dev_key, 16);
    if (s_prov_cb) {
        s_prov_cb(&res, s_prov_ctx);
    }
    s_prov_cb = NULL;
}

// best-effort, node may not respond, local cleanup is what unblocks retries
static void send_node_reset(uint16_t addr)
{
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    esp_ble_mesh_client_common_param_t common = {
        .opcode = ESP_BLE_MESH_MODEL_OP_NODE_RESET,
        .model = &root_models_gateway[1],
        .ctx =
            {
                .net_idx = DP_NET_IDX,
                .app_idx = ESP_BLE_MESH_KEY_UNUSED,
                .addr = addr,
                .send_ttl = 7,
            },
        .msg_timeout = 1000,
    };
    esp_err_t err = esp_ble_mesh_config_client_set_state(&common, &set);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "node-reset send addr=0x%04x err=%d", addr, err);
    }
}

static void prov_finish_err(const char *code, const char *msg)
{
    if (!s_prov_in_flight) {
        return;
    }
    uint16_t target = s_prov_target_addr;
    // flag flip first gates on_cfg_client early-return for any cfg send below
    s_prov_in_flight = false;
    s_cfg_step = CFG_STEP_IDLE;
    if (s_prov_timer) {
        esp_timer_stop(s_prov_timer);
    }
    if (target) {
        // send reset while dev key still in DB, then drop the node so the
        // unicast slot frees and next adoption skips the already-prov check
        send_node_reset(target);
        esp_err_t d = esp_ble_mesh_provisioner_delete_node_with_addr(target);
        if (d != ESP_OK) {
            ESP_LOGW(TAG, "delete node addr=0x%04x err=%d", target, d);
        }
        dp_prov_forget_unicast(target);
    }
    s_prov_target_addr = 0;
    clear_dev_uuid_match();
    dp_mesh_prov_result_t res = {.ok = false, .err_code = code, .err_msg = msg};
    if (s_prov_cb) {
        s_prov_cb(&res, s_prov_ctx);
    }
    s_prov_cb = NULL;
}

esp_err_t dp_mesh_gateway_delete_node(uint16_t unicast_addr)
{
    if (dp_mesh_get_role() != DP_MESH_ROLE_GATEWAY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!unicast_addr) {
        return ESP_ERR_INVALID_ARG;
    }
    // wipe remote node nvs first, else delete only frees the local slot
    // and node stays on mesh with stale keys, never re-enters adoption
    send_node_reset(unicast_addr);
    return esp_ble_mesh_provisioner_delete_node_with_addr(unicast_addr);
}

bool dp_mesh_gateway_has_node_with_uuid(const uint8_t uuid[16])
{
    if (dp_mesh_get_role() != DP_MESH_ROLE_GATEWAY || !uuid) {
        return false;
    }
    return esp_ble_mesh_provisioner_get_node_with_uuid(uuid) != NULL;
}

// ----- post-init. enable prov, push net/app key, bind, group sub -----

esp_err_t dp_mesh_provisioner_post_init(void)
{
    esp_err_t err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);
    if (err != ESP_OK) {
        return err;
    }
    // primary subnet auto-created with random key force ours
    err = esp_ble_mesh_provisioner_update_local_net_key(DP_NET_KEY, DP_NET_IDX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update_local_net_key err=%d", err);
        return err;
    }
    err = esp_ble_mesh_provisioner_add_local_app_key(DP_APP_KEY, DP_NET_IDX, DP_APP_IDX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_local_app_key err=%d", err);
        return err;
    }
    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(DP_GATEWAY_ADDR, DP_APP_IDX,
                                                               DP_VND_MODEL_ID, DP_CID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local bind err=%d", err);
        return err;
    }
    // gateway subscribes the group so it receives sensor publications
    err = esp_ble_mesh_model_subscribe_group_addr(DP_GATEWAY_ADDR, DP_CID, DP_VND_MODEL_ID,
                                                  DP_GROUP_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "group sub err=%d", err);
        return err;
    }
    return ESP_OK;
}

// ----- public entry point -----

esp_err_t dp_mesh_gateway_provision(const uint8_t uuid[16], const uint8_t *static_oob,
                                    uint32_t timeout_ms, dp_mesh_prov_done_cb_t cb, void *ctx)
{
    if (dp_mesh_get_role() != DP_MESH_ROLE_GATEWAY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!uuid || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_prov_in_flight) {
        return ESP_ERR_INVALID_STATE;
    }
    s_prov_in_flight = true;
    s_prov_cb = cb;
    s_prov_ctx = ctx;
    memcpy(s_prov_uuid, uuid, 16);
    s_prov_target_addr = 0;
    s_cfg_step = CFG_STEP_IDLE;
    s_cfg_attempts = 0;

    if (static_oob) {
        esp_err_t err = esp_ble_mesh_provisioner_set_static_oob_value(static_oob, 16);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_static_oob err=%d", err);
            s_prov_in_flight = false;
            return err;
        }
    }

    // match full 16-byte UUID, auto-provision on match
    esp_err_t err = esp_ble_mesh_provisioner_set_dev_uuid_match(uuid, 16, 0, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_dev_uuid_match err=%d", err);
        s_prov_in_flight = false;
        return err;
    }

    if (!s_prov_timer) {
        const esp_timer_create_args_t a = {.callback = prov_timer_cb, .name = "dp_prov_to"};
        esp_timer_create(&a, &s_prov_timer);
    }
    if (s_prov_timer) {
        esp_timer_stop(s_prov_timer);
        esp_timer_start_once(s_prov_timer, (uint64_t)timeout_ms * 1000);
    }
    ESP_LOGI(TAG, "prov start uuid=%02x%02x%02x... timeout=%" PRIu32 "ms", uuid[0], uuid[1],
             uuid[2], timeout_ms);
    emit_state("started");
    return ESP_OK;
}
