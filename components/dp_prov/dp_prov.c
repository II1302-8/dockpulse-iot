#include "dp_prov.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dp_prov";

#define NVS_NAMESPACE "dp_prov"
#define NVS_TABLE_KEY "berths"

// factory partition is mutually exclusive from the default nvs partition
// so factory_reset's nvs_flash_erase() does not wipe per-device claim data
#define FACTORY_NVS_PART  "factory_nvs"
#define FACTORY_NAMESPACE "factory"
#define FACTORY_KEY_OOB   "oob"

#define DP_PROV_MAX_RECORDS 16

typedef struct {
    uint16_t unicast_addr; // 0 = empty
    char berth_id[DP_PROV_BERTH_ID_MAX];
} dp_prov_record_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    uint16_t _pad;
    dp_prov_record_t records[DP_PROV_MAX_RECORDS];
} dp_prov_table_t;

#define DP_PROV_TABLE_VERSION 1

static dp_prov_table_t s_table;
static bool s_loaded;

static esp_err_t load_table(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_table, 0, sizeof(s_table));
        s_table.version = DP_PROV_TABLE_VERSION;
        s_loaded = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(s_table);
    err = nvs_get_blob(h, NVS_TABLE_KEY, &s_table, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND || sz != sizeof(s_table) ||
        s_table.version != DP_PROV_TABLE_VERSION) {
        memset(&s_table, 0, sizeof(s_table));
        s_table.version = DP_PROV_TABLE_VERSION;
    } else if (err != ESP_OK) {
        return err;
    }
    s_loaded = true;
    return ESP_OK;
}

static esp_err_t save_table(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_TABLE_KEY, &s_table, sizeof(s_table));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t dp_prov_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    return load_table();
}

const char *dp_prov_lookup_berth(uint16_t unicast_addr)
{
    if (!s_loaded || unicast_addr == 0) {
        return NULL;
    }
    for (size_t i = 0; i < DP_PROV_MAX_RECORDS; i++) {
        if (s_table.records[i].unicast_addr == unicast_addr) {
            return s_table.records[i].berth_id;
        }
    }
    return NULL;
}

esp_err_t dp_prov_record_berth(uint16_t unicast_addr, const char *berth_id)
{
    if (!unicast_addr || !berth_id || !*berth_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strnlen(berth_id, DP_PROV_BERTH_ID_MAX) >= DP_PROV_BERTH_ID_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_loaded) {
        esp_err_t err = load_table();
        if (err != ESP_OK) {
            return err;
        }
    }
    // hot-swap: drop any stale record bound to the same berth_id under
    // a different unicast addr first
    for (int i = 0; i < DP_PROV_MAX_RECORDS; i++) {
        if (s_table.records[i].unicast_addr != 0 &&
            s_table.records[i].unicast_addr != unicast_addr &&
            strncmp(s_table.records[i].berth_id, berth_id, DP_PROV_BERTH_ID_MAX) == 0) {
            ESP_LOGI(TAG, "hot-swap evict old unicast=0x%04x berth=%s",
                     s_table.records[i].unicast_addr, s_table.records[i].berth_id);
            memset(&s_table.records[i], 0, sizeof(s_table.records[i]));
            if (s_table.count) {
                s_table.count--;
            }
        }
    }
    int slot = -1, empty = -1;
    for (int i = 0; i < DP_PROV_MAX_RECORDS; i++) {
        if (s_table.records[i].unicast_addr == unicast_addr) {
            slot = i;
            break;
        }
        if (empty < 0 && s_table.records[i].unicast_addr == 0) {
            empty = i;
        }
    }
    if (slot < 0) {
        if (empty < 0) {
            ESP_LOGE(TAG, "no slot for unicast=0x%04x", unicast_addr);
            return ESP_ERR_NO_MEM;
        }
        slot = empty;
        s_table.count++;
    }
    s_table.records[slot].unicast_addr = unicast_addr;
    strncpy(s_table.records[slot].berth_id, berth_id, DP_PROV_BERTH_ID_MAX - 1);
    s_table.records[slot].berth_id[DP_PROV_BERTH_ID_MAX - 1] = '\0';
    esp_err_t err = save_table();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "record unicast=0x%04x berth=%s", unicast_addr,
                 s_table.records[slot].berth_id);
    }
    return err;
}

esp_err_t dp_prov_forget_unicast(uint16_t unicast_addr)
{
    if (!s_loaded) {
        esp_err_t err = load_table();
        if (err != ESP_OK) {
            return err;
        }
    }
    for (int i = 0; i < DP_PROV_MAX_RECORDS; i++) {
        if (s_table.records[i].unicast_addr == unicast_addr) {
            memset(&s_table.records[i], 0, sizeof(s_table.records[i]));
            if (s_table.count) {
                s_table.count--;
            }
            return save_table();
        }
    }
    return ESP_OK;
}

void dp_prov_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: wiping NVS");
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    // wipes BLE Mesh stack NVS too (lives in same partition under bt_mesh* namespaces)
    nvs_flash_erase();
    esp_restart();
}

esp_err_t dp_prov_get_static_oob(uint8_t out[DP_PROV_OOB_LEN])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    // partition init is idempotent. fails NOT_FOUND if factory tool never ran
    esp_err_t err = nvs_flash_init_partition(FACTORY_NVS_PART);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(FACTORY_NVS_PART, FACTORY_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    size_t sz = DP_PROV_OOB_LEN;
    err = nvs_get_blob(h, FACTORY_KEY_OOB, out, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (sz != DP_PROV_OOB_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t dp_prov_get_dev_uuid(uint8_t out[DP_PROV_UUID_LEN])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }
    // 6 bytes MAC + 10-byte marker so unprov beacons are easy to filter
    memset(out, 0, DP_PROV_UUID_LEN);
    memcpy(out, mac, sizeof(mac));
    static const uint8_t MARKER[10] = {'D', 'O', 'C', 'K', 'P', 'U', 'L', 'S', 'E', 0x01};
    memcpy(out + 6, MARKER, sizeof(MARKER));
    return ESP_OK;
}
