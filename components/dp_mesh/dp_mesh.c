// esp_ble_mesh on NimBLE host. NimBLE-mesh is broken (per-buffer adv
// events never ble_npl_event_init'd legacy adv worker gated #ifdef MYNEWT)
//
// hub-and-spoke topology. sensor=NODE gateway=PROVISIONER. real PB-ADV
// kicked off by backend MQTT provision/req. cfg client binds AppKey
// and sets pub addr. SETTINGS=y restores everything across reboots

#include "dp_mesh.h"
#include "dp_mesh_priv.h"

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
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"

#include "dp_prov.h"

void ble_store_config_init(void);

static const char *TAG = "dp_mesh";

const uint8_t DP_NET_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'n', 'e', 't', 'k', 'e', 'y',
};
const uint8_t DP_APP_KEY[16] = {
    'd', 'o', 'c', 'k', 'p', 'u', 'l', 's', 'e', '-', 'a', 'p', 'p', 'k', 'e', 'y',
};

static dp_mesh_role_t s_role;
static SemaphoreHandle_t s_bt_sync;
static uint8_t s_dev_uuid[16];
static uint8_t s_own_addr_type;

dp_mesh_role_t dp_mesh_get_role(void) { return s_role; }

// ----- mesh model + element definitions -----

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

esp_ble_mesh_model_t root_models_gateway[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
    ESP_BLE_MESH_MODEL_CFG_CLI(&cfg_cli),
};

static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(DP_OP_STATUS_PUB, BERTH_STATUS_WIRE_LEN),
    ESP_BLE_MESH_MODEL_OP(DP_OP_DIAG_PUB, BERTH_DIAG_WIRE_LEN),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vnd_pub, DP_PUB_BUF_LEN, ROLE_NODE);

esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(DP_CID, DP_VND_MODEL_ID, vnd_op, &vnd_pub, NULL),
};

void dp_mesh_internal_arm_sensor_pub(void)
{
    vnd_pub.publish_addr = DP_GROUP_ADDR;
    vnd_pub.app_idx = DP_APP_IDX;
    vnd_pub.ttl = 7;
    vnd_pub.period = 0;
}

bool dp_mesh_internal_sensor_pub_armed(void)
{
    return vnd_pub.publish_addr != ESP_BLE_MESH_ADDR_UNASSIGNED;
}

esp_err_t dp_mesh_internal_publish(uint32_t opcode, const uint8_t *buf, uint16_t len)
{
    return esp_ble_mesh_model_publish(&vnd_models[0], opcode, len, (uint8_t *)buf, ROLE_NODE);
}

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

// ----- BLE host bring-up -----

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

// ----- mesh-stack callback dispatchers -----

static void on_prov(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_PROV_REGISTER_COMP_EVT) {
        ESP_LOGI(TAG, "prov register err=%d", param->prov_register_comp.err_code);
        return;
    }
    if (s_role == DP_MESH_ROLE_GATEWAY) {
        dp_mesh_provisioner_handle_prov_event(event, param);
    } else {
        dp_mesh_sensor_handle_prov_event(event, param);
    }
}

static void on_model(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        ESP_LOGD(TAG, "model rx opcode=0x%06" PRIx32 " src=0x%04x len=%u",
                 param->model_operation.opcode, param->model_operation.ctx->addr,
                 param->model_operation.length);
        if (s_role == DP_MESH_ROLE_GATEWAY) {
            dp_mesh_provisioner_handle_model_op(param);
        }
        break;
    case ESP_BLE_MESH_MODEL_PUBLISH_COMP_EVT:
        ESP_LOGD(TAG, "publish comp err=%d", param->model_publish_comp.err_code);
        break;
    default:
        break;
    }
}

// ----- init -----

static void make_dev_uuid(void)
{
    if (s_role == DP_MESH_ROLE_SENSOR) {
        // factory QR-encoded UUID. matches sensor beacon to backend claim JWT
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
    if (s_role == DP_MESH_ROLE_SENSOR) {
        dp_mesh_sensor_set_callbacks(cfg->status_cb, cfg->diag_cb, cfg->sensor_ready);
    } else {
        dp_mesh_provisioner_set_callbacks(cfg->status_cb, cfg->diag_cb);
    }
    make_dev_uuid();

    // prov_unicast_addr / prov_start_address are const fields. need
    // designated init + memcpy to populate
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
        // factory-flashed devices carry a per-device random OOB in
        // factory_nvs. bench builds (no factory tool ran) fall back to a
        // shared dev OOB so two-board testing still works without infra
        static uint8_t s_static_oob[16];
        static const uint8_t DEV_FALLBACK_OOB[16] = {
            'd', 'p', '-', 's', 't', 'a', 't', 'i', 'c', '-', 'o', 'o', 'b', 0, 0, 0,
        };
        esp_err_t oob_err = dp_prov_get_static_oob(s_static_oob);
        if (oob_err == ESP_OK) {
            ESP_LOGI(TAG, "factory OOB loaded");
        } else {
            ESP_LOGW(TAG, "factory OOB unavailable (err=%d), using dev fallback", oob_err);
            memcpy(s_static_oob, DEV_FALLBACK_OOB, sizeof(s_static_oob));
        }
        const esp_ble_mesh_prov_t init = {
            .uuid = s_dev_uuid,
            .static_val = s_static_oob,
            .static_val_len = sizeof(s_static_oob),
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
    err = esp_ble_mesh_register_config_server_callback(dp_mesh_sensor_on_cfg_server);
    if (err != ESP_OK) {
        return err;
    }
    if (s_role == DP_MESH_ROLE_GATEWAY) {
        err = dp_mesh_provisioner_register();
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
        err = dp_mesh_provisioner_post_init();
        if (err != ESP_OK) {
            return err;
        }
    } else {
        if (esp_ble_mesh_node_is_provisioned()) {
            // restored from NVS bindings + pub already set
            dp_mesh_sensor_fire_ready();
        } else {
            err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "node_prov_enable err=%d", err);
                return err;
            }
        }
    }

    // log UUID hex for sim-adopt copy-paste
    ESP_LOGI(TAG,
             "ready role=%s uuid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             s_role == DP_MESH_ROLE_GATEWAY ? "gateway" : "sensor", s_dev_uuid[0], s_dev_uuid[1],
             s_dev_uuid[2], s_dev_uuid[3], s_dev_uuid[4], s_dev_uuid[5], s_dev_uuid[6],
             s_dev_uuid[7], s_dev_uuid[8], s_dev_uuid[9], s_dev_uuid[10], s_dev_uuid[11],
             s_dev_uuid[12], s_dev_uuid[13], s_dev_uuid[14], s_dev_uuid[15]);
    return ESP_OK;
}
