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
// backend node_id is a uuid (36 chars + null)
#define DP_PROV_NODE_ID_MAX  40

// adoption metadata. sensor side none (mesh stack handles via SETTINGS=y).
// gateway stores unicast_addr -> {berth_id, node_id} from backend provision/req.
// node_id is the backend-assigned uuid the gateway echoes in every status so
// the backend can bind uplinks to the canonical Node row

esp_err_t dp_prov_init(void);

// NULL if unknown. ptr invalid after next record/forget
const char *dp_prov_lookup_berth(uint16_t unicast_addr);
const char *dp_prov_lookup_node_id(uint16_t unicast_addr);

// insert or overwrite. hot-swap reuses berth_id with new addr.
// node_id may be NULL/empty for pre-rollout firmware paths; backend tolerates
// missing field via the events.py informational fallback
esp_err_t dp_prov_record_node(uint16_t unicast_addr, const char *berth_id,
                              const char *node_id);

esp_err_t dp_prov_forget_unicast(uint16_t unicast_addr);

// wipe dp_prov + mesh NVS reboot. does not return on success
void dp_prov_factory_reset(void);

// MAC-derived stable UUID. factory QR encodes the same value
esp_err_t dp_prov_get_dev_uuid(uint8_t out[DP_PROV_UUID_LEN]);

// per-device static OOB written to factory_nvs by tools/factory-flash.py.
// returns ESP_ERR_NOT_FOUND if device wasn't factory-flashed (bench builds);
// caller falls back to a development OOB
#define DP_PROV_OOB_LEN 16
esp_err_t dp_prov_get_static_oob(uint8_t out[DP_PROV_OOB_LEN]);

#ifdef __cplusplus
}
#endif
