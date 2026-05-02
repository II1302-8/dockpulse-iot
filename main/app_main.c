#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "dp_io.h"
#include "dp_prov.h"

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

static void on_factory_reset(void *ctx)
{
    (void)ctx;
    dp_led_set(DP_LED_ERROR);
    dp_prov_factory_reset();
}

void app_main(void)
{
    init_nvs();
    dp_prov_init();
    dp_led_init();
    dp_button_init(on_factory_reset, NULL);
    dp_led_set(DP_LED_IDLE);

#if CONFIG_DOCKPULSE_ROLE_SENSOR
    ESP_LOGI(TAG, "boot: role=sensor");
    dp_sensor_run();
#elif CONFIG_DOCKPULSE_ROLE_GATEWAY
    ESP_LOGI(TAG, "boot: role=gateway");
    dp_gateway_run();
#else
#error "Select DOCKPULSE_ROLE via menuconfig"
#endif
}
