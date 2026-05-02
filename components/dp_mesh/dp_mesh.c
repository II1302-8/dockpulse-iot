// BLE Mesh wrapper using esp_ble_mesh on NimBLE host. (NimBLE-mesh is
// broken: per-buffer adv events never get ble_npl_event_init'd, legacy
// adv worker is gated #ifdef MYNEWT, IDF blemesh example doesn't
// exercise pub/sub.)
//
// Topology: hub-and-spoke. Sensor=NODE, gateway=PROVISIONER. Real
// PB-ADV: sensor advertises unprov beacon on first boot, gateway
// provisions on demand from MQTT request, then binds AppKey + sets
// publication via cfg client. Subsequent boots restore from NVS via
// CONFIG_BLE_MESH_SETTINGS=y.
//
// Earlier self-provisioning hack (both sides as provisioners with
// shared keys + phantom-peer registration) is gone — see git for the
// archaeology.

#include "dp_mesh.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"

#include "dp_prov.h"
#include "sdkconfig.h"

void ble_store_config_init(void);

static const char *TAG = "dp_mesh";

#define DP_CID           0x02E5
#define DP_VND_MODEL_ID  0x0001
#define DP_OP_STATUS_PUB ESP_BLE_MESH_MODEL_OP_3(0x01, DP_CID)
#define DP_OP_DIAG_PUB   ESP_BLE_MESH_MODEL_OP_3(0x02, DP_CID)
#define DP_PUB_BUF_LEN   (BERTH_DIAG_WIRE_LEN + 3)

#define DP_NET_IDX         0x0000
#define DP_APP_IDX         0x0000
#define DP_GATEWAY_ADDR    0x0001
#define DP_GROUP_ADDR      0xC000
#define DP_MAX_SENSORS     8
#define DP_PROV_START_ADDR (DP_GATEWAY_ADDR + 1)

// Static keys remain — backend doesn't need rotated keys for the
// prototype, and PB-ADV still establishes a unique per-device DevKey.
// NetKey/AppKey are pushed onto each new node by the provisioner.
static const uint8_t DP_NET_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'n', 'e', 't', 'k', 'e', 'y',
};
static const uint8_t DP_APP_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'a', 'p', 'p', 'k', 'e', 'y',
};

static dp_mesh_role_t s_role;
static dp_mesh_status_handler_t s_status_cb;
static dp_mesh_diag_handler_t s_diag_cb;
static dp_mesh_sensor_ready_cb_t s_ready_cb;
static SemaphoreHandle_t s_bt_sync;
static uint16_t s_local_addr;
static uint8_t s_dev_uuid[16];
static uint8_t s_own_addr_type;
static bool s_sensor_ready_fired;

// gateway provisioning state
static dp_mesh_prov_done_cb_t s_prov_cb;
static void *s_prov_ctx;
static uint8_t s_prov_uuid[16];
static bool s_prov_in_flight;
static uint16_t s_prov_target_addr;
static uint8_t s_prov_dev_key[16];
static esp_timer_handle_t s_prov_timer;

// cfg client step machine for post-PB-ADV configuration
typedef enum {
    CFG_STEP_IDLE = 0,
    CFG_STEP_APP_KEY_ADD,
    CFG_STEP_MODEL_APP_BIND,
    CFG_STEP_MODEL_PUB_SET,
    CFG_STEP_DONE,
} cfg_step_t;
static cfg_step_t s_cfg_step;

static void prov_finish_ok(uint16_t addr, const uint8_t dev_key[16]);
static void prov_finish_err(const char *code, const char *msg);
static void cfg_step_advance(uint16_t addr);

static esp_ble_mesh_cfg_srv_t cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

// gateway-only cfg client model
static esp_ble_mesh_client_t cfg_cli;

static esp_ble_mesh_model_t root_models_sensor[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
};

static esp_ble_mesh_model_t root_models_gateway[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
    ESP_BLE_MESH_MODEL_CFG_CLI(&cfg_cli),
};

static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(DP_OP_STATUS_PUB, BERTH_STATUS_WIRE_LEN),
    ESP_BLE_MESH_MODEL_OP(DP_OP_DIAG_PUB, BERTH_DIAG_WIRE_LEN),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vnd_pub, DP_PUB_BUF_LEN, ROLE_NODE);

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(DP_CID, DP_VND_MODEL_ID, vnd_op, &vnd_pub, NULL),
};

// element built per role at init
static esp_ble_mesh_elem_t elements_sensor[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models_sensor, vnd_models),
};
static esp_ble_mesh_elem_t elements_gateway[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models_gateway, vnd_models),
};

static esp_ble_mesh_comp_t comp_sensor = {
    .cid = DP_CID,
    .element_count = 1,
    .elements = elements_sensor,
};
static esp_ble_mesh_comp_t comp_gateway = {
    .cid = DP_CID,
    .element_count = 1,
    .elements = elements_gateway,
};

static esp_ble_mesh_prov_t prov_cfg;

static void on_ble_reset(int reason) { ESP_LOGW(TAG, "ble host reset reason=%d", reason); }

static void on_ble_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }
    xSemaphoreGive(s_bt_sync);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t bt_host_init(void)
{
    s_bt_sync = xSemaphoreCreateBinary();
    if (!s_bt_sync) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init err=%d", ret);
        return ret;
    }
    ble_hs_cfg.reset_cb = on_ble_reset;
    ble_hs_cfg.sync_cb = on_ble_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    if (xSemaphoreTake(s_bt_sync, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "ble host sync timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// ----- prov + model event callbacks -----

static void fire_sensor_ready(void)
{
    if (s_sensor_ready_fired) {
        return;
    }
    uint16_t addr = esp_ble_mesh_get_primary_element_address();
    s_local_addr = addr;
    vnd_pub.publish_addr = DP_GROUP_ADDR;
    vnd_pub.app_idx = DP_APP_IDX;
    vnd_pub.ttl = 7;
    vnd_pub.period = 0;
    s_sensor_ready_fired = true;
    ESP_LOGI(TAG, "sensor ready addr=0x%04x", addr);
    if (s_ready_cb) {
        s_ready_cb(addr);
    }
}

static void on_prov(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "prov register err=%d", param->prov_register_comp.err_code);
        break;

    // sensor (NODE)
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
        // wait for cfg server to receive AppKey Add + Model App Bind
        // before firing ready — see on_cfg_server below
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGW(TAG, "node prov reset");
        break;

    // gateway (PROVISIONER)
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "gw prov enable err=%d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "gw netkey added err=%d", param->provisioner_add_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "gw netkey updated err=%d", param->provisioner_update_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "gw appkey added err=%d", param->provisioner_add_app_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "gw local bind err=%d",
                 param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *u = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        ESP_LOGD(TAG, "unprov beacon uuid=%02x%02x%02x...", u[0], u[1], u[2]);
        // matching via set_dev_uuid_match auto-triggers provisioning,
        // nothing to do here besides log
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "gw prov link open bearer=%d", param->provisioner_prov_link_open.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGW(TAG, "gw prov link close bearer=%d reason=%d",
                 param->provisioner_prov_link_close.bearer,
                 param->provisioner_prov_link_close.reason);
        if (s_prov_in_flight && s_cfg_step < CFG_STEP_APP_KEY_ADD) {
            prov_finish_err("link-close", NULL);
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t addr = param->provisioner_prov_complete.unicast_addr;
        ESP_LOGI(TAG, "gw prov complete addr=0x%04x node_idx=%u", addr,
                 param->provisioner_prov_complete.node_idx);
        s_prov_target_addr = addr;
        const esp_ble_mesh_node_t *node = esp_ble_mesh_provisioner_get_node_with_addr(addr);
        if (node) {
            memcpy(s_prov_dev_key, node->dev_key, 16);
        } else {
            ESP_LOGW(TAG, "node lookup failed for addr=0x%04x", addr);
            memset(s_prov_dev_key, 0, 16);
        }
        cfg_step_advance(addr);
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "gw uuid-match set err=%d",
                 param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_STATIC_OOB_VALUE_COMP_EVT:
        ESP_LOGI(TAG, "gw static-oob set err=%d",
                 param->provisioner_set_static_oob_val_comp.err_code);
        break;

    default:
        ESP_LOGD(TAG, "prov event %d", (int)event);
        break;
    }
}

static void on_model(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT: {
        ESP_LOGD(TAG, "model rx opcode=0x%06" PRIx32 " src=0x%04x len=%u",
                 param->model_operation.opcode, param->model_operation.ctx->addr,
                 param->model_operation.length);
        if (s_role != DP_MESH_ROLE_GATEWAY) {
            break;
        }
        if (param->model_operation.opcode == DP_OP_STATUS_PUB) {
            berth_status_t s;
            if (berth_status_unpack(param->model_operation.msg, param->model_operation.length,
                                    &s) != ESP_OK) {
                ESP_LOGW(TAG, "rx status unpack fail len=%u", param->model_operation.length);
                break;
            }
            if (s_status_cb) {
                s_status_cb(&s, param->model_operation.ctx->addr);
            }
        } else if (param->model_operation.opcode == DP_OP_DIAG_PUB) {
            berth_diag_t d;
            if (berth_diag_unpack(param->model_operation.msg, param->model_operation.length, &d) !=
                ESP_OK) {
                ESP_LOGW(TAG, "rx diag unpack fail len=%u", param->model_operation.length);
                break;
            }
            if (s_diag_cb) {
                s_diag_cb(&d, param->model_operation.ctx->addr);
            }
        }
        break;
    }
    case ESP_BLE_MESH_MODEL_PUBLISH_COMP_EVT:
        ESP_LOGD(TAG, "publish comp err=%d", param->model_publish_comp.err_code);
        break;
    default:
        break;
    }
}

// cfg server callback fires on the SENSOR side when the provisioner
// pushes AppKey Add / Model App Bind / Model Pub Set messages
static void on_cfg_server(esp_ble_mesh_cfg_server_cb_event_t event,
                          esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        return;
    }
    uint32_t op = param->ctx.recv_op;
    ESP_LOGI(TAG, "cfg srv state-change op=0x%04" PRIx32, op);
    if (op == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND || op == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        // bind or pub-set received — vendor model can publish now
        fire_sensor_ready();
    }
}

// cfg client callback fires on the GATEWAY side after each
// AppKeyAdd/ModelAppBind/ModelPubSet response
static void on_cfg_client(esp_ble_mesh_cfg_client_cb_event_t event,
                          esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!s_prov_in_flight) {
        return;
    }
    int err = param->error_code;
    uint32_t op = param->params ? param->params->opcode : 0;
    ESP_LOGI(TAG, "cfg cli evt=%d op=0x%04" PRIx32 " err=%d", (int)event, op, err);
    if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT || err != 0) {
        prov_finish_err("cfg-fail", NULL);
        return;
    }
    if (event == ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT) {
        // ignore — publish notifications, not response to our cmd
        return;
    }
    cfg_step_advance(s_prov_target_addr);
}

// ----- gateway provisioning state machine -----

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
    s_prov_in_flight = false;
    s_cfg_step = CFG_STEP_IDLE;
    if (s_prov_timer) {
        esp_timer_stop(s_prov_timer);
    }
    dp_mesh_prov_result_t res = {.ok = true, .unicast_addr = addr};
    memcpy(res.dev_key, dev_key, 16);
    if (s_prov_cb) {
        s_prov_cb(&res, s_prov_ctx);
    }
    s_prov_cb = NULL;
}

static void prov_finish_err(const char *code, const char *msg)
{
    if (!s_prov_in_flight) {
        return;
    }
    s_prov_in_flight = false;
    s_cfg_step = CFG_STEP_IDLE;
    if (s_prov_timer) {
        esp_timer_stop(s_prov_timer);
    }
    dp_mesh_prov_result_t res = {.ok = false, .err_code = code, .err_msg = msg};
    if (s_prov_cb) {
        s_prov_cb(&res, s_prov_ctx);
    }
    s_prov_cb = NULL;
}

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
        .msg_timeout = 5000,
    };
    return esp_ble_mesh_config_client_set_state(&common, set);
}

static void cfg_step_advance(uint16_t addr)
{
    s_cfg_step++;
    esp_err_t err;
    switch (s_cfg_step) {
    case CFG_STEP_APP_KEY_ADD: {
        esp_ble_mesh_cfg_client_set_state_t set = {0};
        set.app_key_add.net_idx = DP_NET_IDX;
        set.app_key_add.app_idx = DP_APP_IDX;
        memcpy(set.app_key_add.app_key, DP_APP_KEY, 16);
        err = cfg_send(addr, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD, &set);
        if (err != ESP_OK) {
            prov_finish_err("appkey-send", NULL);
        }
        break;
    }
    case CFG_STEP_MODEL_APP_BIND: {
        esp_ble_mesh_cfg_client_set_state_t set = {0};
        set.model_app_bind.element_addr = addr;
        set.model_app_bind.model_app_idx = DP_APP_IDX;
        set.model_app_bind.model_id = DP_VND_MODEL_ID;
        set.model_app_bind.company_id = DP_CID;
        err = cfg_send(addr, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND, &set);
        if (err != ESP_OK) {
            prov_finish_err("bind-send", NULL);
        }
        break;
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
        err = cfg_send(addr, ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET, &set);
        if (err != ESP_OK) {
            prov_finish_err("pubset-send", NULL);
        }
        break;
    }
    case CFG_STEP_DONE:
        prov_finish_ok(addr, s_prov_dev_key);
        break;
    default:
        break;
    }
}

esp_err_t dp_mesh_gateway_provision(const uint8_t uuid[16], const uint8_t *static_oob,
                                    uint32_t timeout_ms, dp_mesh_prov_done_cb_t cb, void *ctx)
{
    if (s_role != DP_MESH_ROLE_GATEWAY) {
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
    return ESP_OK;
}

// ----- init -----

static void make_dev_uuid(void)
{
    if (s_role == DP_MESH_ROLE_SENSOR) {
        // factory QR-encoded UUID — match between sensor beacon and
        // backend's claim JWT
        if (dp_prov_get_dev_uuid(s_dev_uuid) == ESP_OK) {
            return;
        }
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    memset(s_dev_uuid, 0, sizeof(s_dev_uuid));
    memcpy(s_dev_uuid, mac, sizeof(mac));
    s_dev_uuid[6] = (uint8_t)s_role;
}

esp_err_t dp_mesh_init(const dp_mesh_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_role = cfg->role;
    s_status_cb = cfg->status_cb;
    s_diag_cb = cfg->diag_cb;
    s_ready_cb = cfg->sensor_ready;
    make_dev_uuid();

    // prov_unicast_addr / prov_start_address are const members — must
    // initialize via designated init then memcpy
    if (s_role == DP_MESH_ROLE_GATEWAY) {
        const esp_ble_mesh_prov_t init = {
            .uuid = s_dev_uuid,
            .prov_uuid = s_dev_uuid,
            .prov_unicast_addr = DP_GATEWAY_ADDR,
            .prov_start_address = DP_PROV_START_ADDR,
        };
        memcpy(&prov_cfg, &init, sizeof(prov_cfg));
    } else {
        // gateway pushes matching OOB from backend QR JWT before each
        // flow via set_static_oob_value
        static const uint8_t SENSOR_STATIC_OOB[16] = {
            'd', 'p', '-', 's', 't', 'a', 't', 'i', 'c', '-', 'o', 'o', 'b', 0, 0, 0,
        };
        const esp_ble_mesh_prov_t init = {
            .uuid = s_dev_uuid,
            .static_val = SENSOR_STATIC_OOB,
            .static_val_len = 16,
        };
        memcpy(&prov_cfg, &init, sizeof(prov_cfg));
    }

    esp_err_t err = bt_host_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_register_prov_callback(on_prov);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_register_custom_model_callback(on_model);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_register_config_server_callback(on_cfg_server);
    if (err != ESP_OK) {
        return err;
    }
    if (s_role == DP_MESH_ROLE_GATEWAY) {
        err = esp_ble_mesh_register_config_client_callback(on_cfg_client);
        if (err != ESP_OK) {
            return err;
        }
    }

    err =
        esp_ble_mesh_init(&prov_cfg, s_role == DP_MESH_ROLE_GATEWAY ? &comp_gateway : &comp_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init err=%d", err);
        return err;
    }

    if (s_role == DP_MESH_ROLE_GATEWAY) {
        err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);
        if (err != ESP_OK) {
            return err;
        }
        // primary subnet auto-created with random key; force ours
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
        s_local_addr = DP_GATEWAY_ADDR;
    } else {
        if (esp_ble_mesh_node_is_provisioned()) {
            // restored from NVS — bindings + pub already in cfg_srv state
            fire_sensor_ready();
        } else {
            err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "node_prov_enable err=%d", err);
                return err;
            }
        }
    }

    // log UUID hex so bench tests can copy-paste it into mosquitto_pub
    ESP_LOGI(TAG, "ready role=%s uuid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             s_role == DP_MESH_ROLE_GATEWAY ? "gateway" : "sensor",
             s_dev_uuid[0], s_dev_uuid[1], s_dev_uuid[2], s_dev_uuid[3],
             s_dev_uuid[4], s_dev_uuid[5], s_dev_uuid[6], s_dev_uuid[7],
             s_dev_uuid[8], s_dev_uuid[9], s_dev_uuid[10], s_dev_uuid[11],
             s_dev_uuid[12], s_dev_uuid[13], s_dev_uuid[14], s_dev_uuid[15]);
    return ESP_OK;
}

esp_err_t dp_mesh_publish_status(const berth_status_t *s)
{
    if (!s || s_role != DP_MESH_ROLE_SENSOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sensor_ready_fired || vnd_pub.publish_addr == ESP_BLE_MESH_ADDR_UNASSIGNED) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t wire[BERTH_STATUS_WIRE_LEN];
    size_t wire_len = 0;
    esp_err_t err = berth_status_pack(s, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_model_publish(&vnd_models[0], DP_OP_STATUS_PUB, (uint16_t)wire_len, wire,
                                     ROLE_NODE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish err=%d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t dp_mesh_publish_diag(const berth_diag_t *d)
{
    if (!d || s_role != DP_MESH_ROLE_SENSOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sensor_ready_fired || vnd_pub.publish_addr == ESP_BLE_MESH_ADDR_UNASSIGNED) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t wire[BERTH_DIAG_WIRE_LEN];
    size_t wire_len = 0;
    esp_err_t err = berth_diag_pack(d, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_model_publish(&vnd_models[0], DP_OP_DIAG_PUB, (uint16_t)wire_len, wire,
                                     ROLE_NODE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish diag err=%d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}
