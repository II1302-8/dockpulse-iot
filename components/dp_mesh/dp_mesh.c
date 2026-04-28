// BLE Mesh wrapper using Espressif's esp_ble_mesh stack on the NimBLE
// host. We deliberately avoid the in-tree NimBLE-mesh component (under
// `bt/host/nimble/.../mesh/`) because its FreeRTOS port is incomplete:
// per-buffer adv events are never `ble_npl_event_init`'d, the legacy
// adv worker thread is gated `#ifdef MYNEWT`, and the IDF reference
// `blemesh` example never exercises publish/subscribe — so the bug
// surface is wide.
//
// Topology: hub-and-spoke. Sensor nodes publish a packed
// `berth_status_t` on a shared vendor model; the gateway subscribes to
// the same group address and decodes each message into a
// `dp_mesh_status_handler_t` callback.
//
// Self-provisioning: each node runs as its own one-device "provisioner"
// (esp_ble_mesh provisioner role) and uses the local-data API to add a
// shared NetKey/AppKey, bind the AppKey to the vendor model, and set
// publication or subscription. There is no on-air provisioning
// handshake — every node is hard-coded with the same keys and a
// deterministic unicast address derived from CONFIG_DOCKPULSE_NODE_ID.
// Fine for the prototype; not suitable for production (no auth, shared
// static keys).

#include "dp_mesh.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
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

void ble_store_config_init(void);

// Internal esp_ble_mesh API for stuffing a phantom node into the
// provisioner's known-nodes DB. Needed because core/net.c:1885 silently
// drops mesh packets whose `src` isn't in that DB when running as a
// provisioner — see comment in dp_mesh_init() near register_phantom_peer.
// `bt_mesh_addr_t` is pulled in transitively via esp_ble_mesh_defs.h →
// proxy_server.h → mesh/adapter.h, so we only need the function decl.
extern int bt_mesh_provisioner_provision(const bt_mesh_addr_t *addr, const uint8_t uuid[16],
                                         uint16_t oob_info, uint16_t unicast_addr,
                                         uint8_t element_num, uint16_t net_idx, uint8_t flags,
                                         uint32_t iv_index, const uint8_t dev_key[16],
                                         uint16_t *index, bool nppi);

static const char *TAG = "dp_mesh";

#define DP_CID           0x02E5
#define DP_VND_MODEL_ID  0x0001
#define DP_OP_STATUS_PUB ESP_BLE_MESH_MODEL_OP_3(0x01, DP_CID)

#define DP_NET_IDX      0x0000
#define DP_APP_IDX      0x0000
#define DP_GATEWAY_ADDR 0x0001
#define DP_GROUP_ADDR   0xC000
// Pre-registered sensor address range on the gateway. Must be <=
// CONFIG_BLE_MESH_MAX_PROV_NODES (default 10).
#define DP_MAX_SENSORS 8

static const uint8_t DP_NET_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'n', 'e', 't', 'k', 'e', 'y',
};
static const uint8_t DP_APP_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'a', 'p', 'p', 'k', 'e', 'y',
};

static dp_mesh_role_t s_role;
static dp_mesh_status_handler_t s_handler;
static SemaphoreHandle_t s_bt_sync;
static uint16_t s_local_addr;
static uint8_t s_dev_uuid[16];
static uint8_t s_own_addr_type;

static esp_ble_mesh_cfg_srv_t cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
};

static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(DP_OP_STATUS_PUB, BERTH_STATUS_WIRE_LEN),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vnd_pub, BERTH_STATUS_WIRE_LEN + 3, ROLE_PROVISIONER);

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(DP_CID, DP_VND_MODEL_ID, vnd_op, &vnd_pub, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t comp = {
    .cid = DP_CID,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

// Filled in at dp_mesh_init time — `prov_unicast_addr` is a const
// member so we can't poke it after construction. We memcpy a fully
// designated initializer into this storage on init.
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

static void on_prov(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "prov register err=%d", param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "provisioner enabled err=%d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "local netkey added err=%d", param->provisioner_add_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "local netkey updated err=%d",
                 param->provisioner_update_net_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "local appkey added err=%d", param->provisioner_add_app_key_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "appkey bound to vnd model err=%d",
                 param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    case ESP_BLE_MESH_MODEL_SUBSCRIBE_GROUP_ADDR_COMP_EVT:
        ESP_LOGI(TAG, "model subscribed to group=0x%04x err=%d",
                 param->model_sub_group_addr_comp.group_addr,
                 param->model_sub_group_addr_comp.err_code);
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
        ESP_LOGI(TAG, "model rx opcode=0x%06" PRIx32 " src=0x%04x len=%u",
                 param->model_operation.opcode, param->model_operation.ctx->addr,
                 param->model_operation.length);
        if (param->model_operation.opcode == DP_OP_STATUS_PUB && s_role == DP_MESH_ROLE_GATEWAY) {
            berth_status_t s;
            esp_err_t err =
                berth_status_unpack(param->model_operation.msg, param->model_operation.length, &s);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "rx unpack err=%d len=%u", err, param->model_operation.length);
                break;
            }
            if (s_handler) {
                s_handler(&s, param->model_operation.ctx->addr);
            }
        }
        break;
    }
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
        ESP_LOGI(TAG, "model send comp opcode=0x%06" PRIx32 " err=%d",
                 param->model_send_comp.opcode, param->model_send_comp.err_code);
        break;
    case ESP_BLE_MESH_MODEL_PUBLISH_COMP_EVT:
        ESP_LOGI(TAG, "publish comp err=%d", param->model_publish_comp.err_code);
        break;
    case ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT:
        ESP_LOGI(TAG, "client rx publish opcode=0x%06" PRIx32 " src=0x%04x",
                 param->client_recv_publish_msg.opcode, param->client_recv_publish_msg.ctx->addr);
        break;
    default:
        ESP_LOGD(TAG, "model event %d", (int)event);
        break;
    }
}

static void register_phantom_peer(uint16_t addr)
{
    if (addr == s_local_addr) {
        return;
    }
    bt_mesh_addr_t bd_addr = {0};
    uint8_t uuid[16] = {'d', 'p', '-', 'p', 'h', 'a', 'n', 't', 'o', 'm', 0};
    uuid[14] = (uint8_t)(addr & 0xFF);
    uuid[15] = (uint8_t)(addr >> 8);
    uint8_t dev_key[16] = {0}; // unused — we never send DEVKEY-encrypted msgs to peers
    uint16_t index = 0;
    int rc = bt_mesh_provisioner_provision(&bd_addr, uuid, 0, addr, 1, DP_NET_IDX, 0, 0, dev_key,
                                           &index, false);
    if (rc) {
        ESP_LOGW(TAG, "phantom peer 0x%04x rc=%d", addr, rc);
    } else {
        ESP_LOGI(TAG, "phantom peer 0x%04x registered idx=%u", addr, index);
    }
}

static void make_dev_uuid(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    memset(s_dev_uuid, 0, sizeof(s_dev_uuid));
    memcpy(s_dev_uuid, mac, sizeof(mac));
    s_dev_uuid[6] = (uint8_t)s_role;
    uint8_t node_id = 0;
    dp_common_get_node_id(&node_id);
    s_dev_uuid[7] = node_id;
}

esp_err_t dp_mesh_init(dp_mesh_role_t role)
{
    s_role = role;
    make_dev_uuid();

    uint8_t node_id = 0;
    dp_common_get_node_id(&node_id);
    s_local_addr =
        (role == DP_MESH_ROLE_GATEWAY) ? DP_GATEWAY_ADDR : (uint16_t)(DP_GATEWAY_ADDR + node_id);

    const esp_ble_mesh_prov_t prov_init = {
        .uuid = s_dev_uuid,
        .prov_uuid = s_dev_uuid,
        .prov_unicast_addr = s_local_addr,
        // prov_start_address must be a valid unicast > prov_unicast_addr
        // (the stack rejects 0xFFFF / group / RFU). We never actually
        // onboard others — provisioner_prov_enable is called only so
        // the local-data API will accept our key/bind calls — but the
        // value still has to pass validation. 0x7FFF is the top of the
        // unicast range, safely past anything we hand out ourselves.
        .prov_start_address = 0x7FFF,
        .prov_attention = 0,
        .prov_algorithm = 0,
        .prov_pub_key_oob = 0,
        .prov_static_oob_val = NULL,
        .prov_static_oob_len = 0,
        .flags = 0,
        .iv_index = 0,
    };
    memcpy(&prov_cfg, &prov_init, sizeof(prov_cfg));

    esp_err_t err = bt_host_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_ble_mesh_register_prov_callback(on_prov);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register prov cb err=%d", err);
        return err;
    }

    err = esp_ble_mesh_register_custom_model_callback(on_model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register model cb err=%d", err);
        return err;
    }

    err = esp_ble_mesh_init(&prov_cfg, &comp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init err=%d", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioner_prov_enable err=%d", err);
        return err;
    }

    // provisioner_prov_enable auto-creates a random primary subnet
    // (net_idx=0). add_local_net_key explicitly rejects net_idx=0 — see
    // esp_ble_mesh_networking_api.c — so to install our deterministic
    // shared key we have to *update* the primary instead of adding a
    // new one.
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

    // Provisioner-side bind: signature is (elem_addr, app_idx, model_id,
    // company_id). The 0xFFFF cid sentinel is the *node*-side bind, a
    // different API in local_data_operation_api.h.
    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(s_local_addr, DP_APP_IDX,
                                                               DP_VND_MODEL_ID, DP_CID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bind_app_key err=%d", err);
        return err;
    }

    if (role == DP_MESH_ROLE_SENSOR) {
        vnd_pub.publish_addr = DP_GROUP_ADDR;
        vnd_pub.app_idx = DP_APP_IDX;
        vnd_pub.ttl = 7;
        vnd_pub.period = 0;

        // The gateway runs as a provisioner; its receive path drops
        // packets whose source unicast isn't in its node DB. The
        // sensor must therefore be registered as a known node on the
        // gateway. We do the inverse here so each side accepts the
        // other's traffic (the gateway itself runs the matching loop
        // below). See bt_mesh_provisioner_get_node_with_addr filter
        // in core/net.c:1885.
        register_phantom_peer(DP_GATEWAY_ADDR);
    } else {
        err = esp_ble_mesh_model_subscribe_group_addr(s_local_addr, DP_CID, DP_VND_MODEL_ID,
                                                      DP_GROUP_ADDR);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "subscribe_group err=%d", err);
            return err;
        }

        // Pre-register every possible sensor address. Inflates the
        // node DB by DP_MAX_SENSORS entries even if some slots are
        // never used; cheap.
        for (uint16_t i = 0; i < DP_MAX_SENSORS; i++) {
            register_phantom_peer(DP_GATEWAY_ADDR + 1 + i);
        }
    }

    ESP_LOGI(TAG, "ready role=%s addr=0x%04x", role == DP_MESH_ROLE_GATEWAY ? "gateway" : "sensor",
             s_local_addr);
    return ESP_OK;
}

esp_err_t dp_mesh_publish_status(const berth_status_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_role != DP_MESH_ROLE_SENSOR) {
        return ESP_ERR_INVALID_STATE;
    }
    if (vnd_pub.publish_addr == ESP_BLE_MESH_ADDR_UNASSIGNED) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t wire[BERTH_STATUS_WIRE_LEN];
    size_t wire_len = 0;
    esp_err_t err = berth_status_pack(s, wire, sizeof(wire), &wire_len);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_ble_mesh_model_publish(&vnd_models[0], DP_OP_STATUS_PUB, (uint16_t)wire_len, wire,
                                     ROLE_PROVISIONER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish err=%d", err);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "published berth_id=%u occupied=%d raw_mm=%u", s->berth_id, s->occupied,
             s->sensor_raw_mm);
    return ESP_OK;
}

esp_err_t dp_mesh_set_status_handler(dp_mesh_status_handler_t cb)
{
    if (s_role != DP_MESH_ROLE_GATEWAY) {
        return ESP_ERR_INVALID_STATE;
    }
    s_handler = cb;
    return ESP_OK;
}
