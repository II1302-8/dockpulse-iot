// BLE Mesh wrapper using NimBLE host (CONFIG_BT_NIMBLE_MESH=y).
//
// Topology: hub-and-spoke. Sensor nodes publish presence/distance via a
// vendor model on a per-installation app key. Gateway subscribes and
// forwards via dp_gateway_uplink().
//
// This is a scaffold — the full provisioning + model registration will
// land alongside the first end-to-end test. The functions below are
// safe no-ops so app_main can link and boot.

#include "dp_mesh.h"

#include "esp_log.h"

static const char *TAG = "dp_mesh";

static dp_mesh_sample_handler_t s_handler;
static dp_mesh_role_t s_role;

esp_err_t dp_mesh_init(dp_mesh_role_t role)
{
    s_role = role;
    ESP_LOGW(TAG, "init role=%d (stub — wire NimBLE mesh provisioning + vendor model)", (int)role);
    // TODO:
    //   1. nimble_port_init() / nimble_port_freertos_init()
    //   2. ble_mesh_init() with provisioning + composition data
    //   3. Register vendor model (CID 0x02E5 for Espressif, or assign your own)
    //      with opcodes for SAMPLE_PUB / SAMPLE_STATUS
    //   4. Call esp_ble_mesh_node_prov_enable() on first boot
    return ESP_OK;
}

esp_err_t dp_mesh_publish_sample(const dp_radar_sample_t *s)
{
    if (!s)
        return ESP_ERR_INVALID_ARG;
    if (s_role != DP_MESH_ROLE_SENSOR)
        return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "publish presence=%d distance_cm=%u (stub)", s->presence, s->distance_cm);
    // TODO: build payload, esp_ble_mesh_model_publish() on vendor model.
    return ESP_OK;
}

esp_err_t dp_mesh_set_sample_handler(dp_mesh_sample_handler_t cb)
{
    if (s_role != DP_MESH_ROLE_GATEWAY)
        return ESP_ERR_INVALID_STATE;
    s_handler = cb;
    return ESP_OK;
}
