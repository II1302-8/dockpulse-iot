#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "dockpulse";

#if CONFIG_DOCKPULSE_ROLE_SENSOR
extern void dp_sensor_run(void);
#endif
#if CONFIG_DOCKPULSE_ROLE_GATEWAY
extern void dp_gateway_run(void);
#endif

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();

#if CONFIG_DOCKPULSE_ROLE_SENSOR
    ESP_LOGI(TAG, "boot: role=sensor node=%d", CONFIG_DOCKPULSE_NODE_ID);
    dp_sensor_run();
#elif CONFIG_DOCKPULSE_ROLE_GATEWAY
    ESP_LOGI(TAG, "boot: role=gateway node=%d", CONFIG_DOCKPULSE_NODE_ID);
    dp_gateway_run();
#else
#error "Select DOCKPULSE_ROLE via menuconfig"
#endif
}
