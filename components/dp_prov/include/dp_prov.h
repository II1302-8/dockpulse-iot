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

// adoption metadata. sensor side none (mesh stack handles via SETTINGS=y).
// gateway stores unicast_addr -> berth_id from backend provision/req

esp_err_t dp_prov_init(void);

// NULL if unknown. ptr invalid after next record/forget
const char *dp_prov_lookup_berth(uint16_t unicast_addr);

// insert or overwrite. hot-swap reuses berth_id with new addr
esp_err_t dp_prov_record_berth(uint16_t unicast_addr, const char *berth_id);

esp_err_t dp_prov_forget_unicast(uint16_t unicast_addr);

// wipe dp_prov + mesh NVS reboot. does not return on success
void dp_prov_factory_reset(void);

// MAC-derived stable UUID. factory QR encodes the same value
esp_err_t dp_prov_get_dev_uuid(uint8_t out[DP_PROV_UUID_LEN]);

#ifdef __cplusplus
}
#endif
