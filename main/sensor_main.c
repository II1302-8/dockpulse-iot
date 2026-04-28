#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dp_common.h"
#include "dp_mesh.h"
#include "dp_radar.h"

static const char *TAG = "sensor";

static berth_status_t to_status(const dp_radar_sample_t *s, uint8_t node_id)
{
    return (berth_status_t){
        .node_id = node_id,
        // 1:1 sensor-to-berth mapping for now. When that changes,
        // route node_id → berth_id through a config lookup here.
        .berth_id = node_id,
        // Until the boat-detection logic from the field-test data
        // lands, fall back to the radar's human-tuned presence flag.
        .occupied = s->presence,
        .sensor_raw_mm = (uint16_t)(s->distance_cm * 10u),
        // Battery monitoring not yet wired (no ADC divider on the
        // current hardware revision).
        .battery_pct = DP_BATTERY_UNKNOWN,
        .ts_ms = s->ts_ms,
    };
}

void dp_sensor_run(void)
{
    ESP_ERROR_CHECK(dp_radar_init());
    ESP_ERROR_CHECK(dp_mesh_init(DP_MESH_ROLE_SENSOR));

    uint8_t node_id = 0;
    dp_common_get_node_id(&node_id);

    while (true) {
        dp_radar_sample_t s;
        esp_err_t err = dp_radar_read(&s, pdMS_TO_TICKS(2000));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "presence=%d distance_cm=%u", s.presence, s.distance_cm);
            // Field-test trace: one CSV-style line per sample so logs can
            // be grepped (`grep ',RADAR,' …`) and fed to a plotter to set
            // per-berth gate thresholds. Drop or gate behind a Kconfig
            // once boat-detection logic is calibrated.
            ESP_LOGI(TAG,
                     "RADAR,%u,%d,%u,"
                     "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                     (unsigned)s.ts_ms, (int)s.presence, (unsigned)s.distance_cm, s.gate_energy[0],
                     s.gate_energy[1], s.gate_energy[2], s.gate_energy[3], s.gate_energy[4],
                     s.gate_energy[5], s.gate_energy[6], s.gate_energy[7], s.gate_energy[8],
                     s.gate_energy[9], s.gate_energy[10], s.gate_energy[11], s.gate_energy[12],
                     s.gate_energy[13], s.gate_energy[14], s.gate_energy[15]);

            berth_status_t status = to_status(&s, node_id);
            dp_mesh_publish_status(&status);
        } else {
            ESP_LOGW(TAG, "radar read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_DOCKPULSE_SENSOR_PERIOD_MS));
        // TODO: replace vTaskDelay with light/deep sleep once mesh + radar
        // shutdown sequencing is in place. Solar power budget needs sleep.
    }
}
