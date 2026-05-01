#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP_PROV_UUID_LEN 16
// "ksss-saltsjobaden-pier-1-t1" ~30 chars; 47 + null leaves headroom
#define DP_PROV_BERTH_ID_MAX 48

// Adoption metadata. Sensor uses none — provisioning state lives in
// the esp_ble_mesh stack NVS (BLE_MESH_SETTINGS=y). Gateway stores
// (unicast_addr -> berth_id) records, set from backend's provision/req
// payload, used by uplink to render MQTT topic.

esp_err_t dp_prov_init(void);

// NULL if unknown. Pointer invalid after next record/forget call.
const char *dp_prov_lookup_berth(uint16_t unicast_addr);

// Insert or overwrite. Hot-swap reuses the same berth_id with a new addr.
esp_err_t dp_prov_record_berth(uint16_t unicast_addr, const char *berth_id);

esp_err_t dp_prov_forget_unicast(uint16_t unicast_addr);

// Wipe dp_prov + ble mesh NVS, reboot. Does not return on success.
void dp_prov_factory_reset(void);

// MAC-derived stable UUID. Factory QR encodes the same value.
esp_err_t dp_prov_get_dev_uuid(uint8_t out[DP_PROV_UUID_LEN]);

#ifdef __cplusplus
}
#endif
