#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dp_gateway.h"
#include "dp_mesh.h"

static const char *TAG = "gateway";

static void on_sample(const dp_radar_sample_t *s, uint16_t src_addr)
{
    ESP_LOGI(TAG, "rx src=0x%04x presence=%d distance_cm=%u", src_addr, s->presence,
             s->distance_cm);
    dp_gateway_uplink(s, src_addr);
}

void dp_gateway_run(void)
{
    ESP_ERROR_CHECK(dp_gateway_init());
    ESP_ERROR_CHECK(dp_mesh_init(DP_MESH_ROLE_GATEWAY));
    ESP_ERROR_CHECK(dp_mesh_set_sample_handler(on_sample));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
