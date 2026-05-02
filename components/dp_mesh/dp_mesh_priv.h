#pragma once

// internal. shared by dp_mesh.c / _sensor.c / _provisioner.c

#include <stdint.h>

#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_provisioning_api.h"

#include "dp_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----- shared mesh constants -----

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

// shared NetKey/AppKey ok for prototype. PB-ADV still gives each
// node a unique DevKey. provisioner pushes these to new nodes
extern const uint8_t DP_NET_KEY[16];
extern const uint8_t DP_APP_KEY[16];

// ----- shared model storage. defined in dp_mesh.c -----

extern esp_ble_mesh_model_t vnd_models[1];
extern esp_ble_mesh_model_t root_models_gateway[2];

// vnd_pub is macro-static. sensor reaches it via these
void dp_mesh_internal_arm_sensor_pub(void);
bool dp_mesh_internal_sensor_pub_armed(void);
esp_err_t dp_mesh_internal_publish(uint32_t opcode, const uint8_t *buf, uint16_t len);

// ----- role accessor (dp_mesh.c owns s_role) -----

dp_mesh_role_t dp_mesh_get_role(void);

// ----- sensor (NODE) side. dp_mesh_sensor.c -----

void dp_mesh_sensor_set_callbacks(dp_mesh_status_handler_t status_cb,
                                  dp_mesh_diag_handler_t diag_cb,
                                  dp_mesh_sensor_ready_cb_t ready_cb);

void dp_mesh_sensor_handle_prov_event(esp_ble_mesh_prov_cb_event_t event,
                                      esp_ble_mesh_prov_cb_param_t *param);

void dp_mesh_sensor_on_cfg_server(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param);

// NVS-restore branch
void dp_mesh_sensor_fire_ready(void);

// ----- provisioner (GATEWAY) side. dp_mesh_provisioner.c -----

// pre-esp_ble_mesh_init. registers cfg-client cb
esp_err_t dp_mesh_provisioner_register(void);

// post-init. enable prov, push net+app key, bind, group sub
esp_err_t dp_mesh_provisioner_post_init(void);

void dp_mesh_provisioner_handle_prov_event(esp_ble_mesh_prov_cb_event_t event,
                                           esp_ble_mesh_prov_cb_param_t *param);

void dp_mesh_provisioner_handle_model_op(esp_ble_mesh_model_cb_param_t *param);

void dp_mesh_provisioner_set_callbacks(dp_mesh_status_handler_t status_cb,
                                       dp_mesh_diag_handler_t diag_cb);

#ifdef __cplusplus
}
#endif
