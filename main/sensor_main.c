#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dp_mesh.h"
#include "dp_radar.h"

static const char *TAG = "sensor";

void dp_sensor_run(void)
{
    ESP_ERROR_CHECK(dp_radar_init());
    ESP_ERROR_CHECK(dp_mesh_init(DP_MESH_ROLE_SENSOR));

    while (true) {
        dp_radar_sample_t s;
        esp_err_t err = dp_radar_read(&s, pdMS_TO_TICKS(2000));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "presence=%d distance_cm=%u", s.presence, s.distance_cm);
            dp_mesh_publish_sample(&s);
        } else {
            ESP_LOGW(TAG, "radar read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_DOCKPULSE_SENSOR_PERIOD_MS));
        // TODO: replace vTaskDelay with light/deep sleep once mesh + radar
        // shutdown sequencing is in place. Solar power budget needs sleep.
    }
}
