#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dp_common.h"
#include "dp_gateway.h"
#include "dp_mesh.h"

static const char *TAG = "gateway";

static void on_status(const berth_status_t *s, uint16_t src_addr)
{
    ESP_LOGI(TAG, "rx src=0x%04x node=%u berth=%u occupied=%d raw_mm=%u", src_addr, s->node_id,
             s->berth_id, s->occupied, s->sensor_raw_mm);
    dp_gateway_uplink(s, src_addr);
}

void dp_gateway_run(void)
{
    ESP_ERROR_CHECK(dp_gateway_init());
    ESP_ERROR_CHECK(dp_mesh_init(DP_MESH_ROLE_GATEWAY));
    ESP_ERROR_CHECK(dp_mesh_set_status_handler(on_status));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
