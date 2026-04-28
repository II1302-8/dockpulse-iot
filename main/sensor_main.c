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

    // The HMMD streams Report frames at ~10 Hz. If we sleep longer than
    // the UART RX buffer can hold (~140 ms at 115200 baud), the buffer
    // ages out into mid-frame garbage and reads time out. So we *read*
    // at the radar's natural cadence and only *publish* on the slower
    // CONFIG_DOCKPULSE_SENSOR_PERIOD_MS schedule.
    //
    // TODO: replace this poll-and-throttle pattern with deep-sleep +
    // OT2 GPIO wakeup once we tackle the solar power budget. Currently
    // CPU runs continuously which is fine for mains-powered bench
    // testing.
    const TickType_t read_interval = pdMS_TO_TICKS(200);
    const TickType_t publish_interval = pdMS_TO_TICKS(CONFIG_DOCKPULSE_SENSOR_PERIOD_MS);
    TickType_t last_publish = 0;

    // The HMMD occasionally drops out of Report mode (silent, no error
    // surfaces). After this many back-to-back read timeouts, we nudge
    // it back by re-sending the mode-change command. ~10 misses ≈ 2 s
    // at the 200 ms read cadence — long enough to skip the radar's
    // own brief calibration pauses without false-positive recovery.
    const int RECOVERY_THRESHOLD = 10;
    int consecutive_failures = 0;

    while (true) {
        // 500ms is plenty: HMMD streams at ~10Hz, so a fresh frame
        // should arrive within 100ms. The radar occasionally pauses
        // for one or two frames (calibration), which is normal — log
        // at DEBUG so the warning isn't noisy in steady state.
        dp_radar_sample_t s;
        esp_err_t err = dp_radar_read(&s, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "radar read skipped: %s", esp_err_to_name(err));
            if (++consecutive_failures == RECOVERY_THRESHOLD) {
                ESP_LOGW(TAG, "no radar frames in %dx reads — re-sending mode-change",
                         RECOVERY_THRESHOLD);
                dp_radar_enter_report_mode();
                consecutive_failures = 0;
            }
            vTaskDelay(read_interval);
            continue;
        }
        consecutive_failures = 0;

        TickType_t now = xTaskGetTickCount();
        if (last_publish == 0 || (now - last_publish) >= publish_interval) {
            ESP_LOGI(TAG, "presence=%d distance_cm=%u", s.presence, s.distance_cm);
            // Field-test trace: one CSV-style line per published sample
            // so logs can be grepped (`grep ',RADAR,' …`) and fed to a
            // plotter to set per-berth gate thresholds.
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
            last_publish = now;
        }

        vTaskDelay(read_interval);
    }
}
