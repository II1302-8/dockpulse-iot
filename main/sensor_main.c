#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dp_common.h"
#include "dp_io.h"
#include "dp_mesh.h"
#include "dp_radar.h"
#include "dp_radar_filter.h"

static const char *TAG = "sensor";

static volatile bool s_mesh_ready;
static volatile uint16_t s_unicast;

static void on_sensor_ready(uint16_t addr)
{
    s_unicast = addr;
    s_mesh_ready = true;
    dp_led_set(DP_LED_OK);
    ESP_LOGI(TAG, "mesh ready addr=0x%04x", addr);
}

static berth_status_t to_status(const dp_radar_sample_t *s, bool occupied, uint8_t node_id,
                                uint16_t berth_id)
{
    return (berth_status_t){
        .node_id = node_id,
        .berth_id = berth_id,
        .occupied = occupied,
        .sensor_raw_mm = (uint16_t)(s->distance_cm * 10u),
        .battery_pct = DP_BATTERY_UNKNOWN,
        .ts_ms = s->ts_ms,
    };
}

#if CONFIG_DOCKPULSE_DIAG_ENABLE
static berth_diag_t to_diag(const dp_radar_sample_t *s, uint8_t node_id, uint16_t berth_id)
{
    berth_diag_t d = {
        .node_id = node_id,
        .berth_id = berth_id,
        .target_state = s->target_state,
        .raw_distance_cm = s->distance_cm,
        .ts_ms = s->ts_ms,
    };
    for (size_t i = 0; i < DP_RADAR_GATE_COUNT; i++) {
        d.gate_energy[i] = s->gate_energy[i];
    }
    return d;
}
#endif

void dp_sensor_run(void)
{
    ESP_ERROR_CHECK(dp_radar_init());

    dp_led_set(DP_LED_PROVISIONING);
    ESP_ERROR_CHECK(dp_mesh_init(&(const dp_mesh_cfg_t){
        .role = DP_MESH_ROLE_SENSOR,
        .sensor_ready = on_sensor_ready,
    }));

    uint8_t node_id = (uint8_t)CONFIG_DOCKPULSE_NODE_ID;

    // HMMD streams Report at ~10Hz; UART RX buffer ages into garbage if
    // we sleep > ~140ms. Read at radar cadence, publish on slower
    // CONFIG_DOCKPULSE_SENSOR_PERIOD_MS.
    const TickType_t read_interval = pdMS_TO_TICKS(200);
    const TickType_t publish_interval = pdMS_TO_TICKS(CONFIG_DOCKPULSE_SENSOR_PERIOD_MS);
    TickType_t last_publish = 0;

    const int RECOVERY_THRESHOLD = 10;
    int consecutive_failures = 0;

    while (true) {
        dp_radar_sample_t s;
        esp_err_t err = dp_radar_read(&s, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "radar skip: %s", esp_err_to_name(err));
            if (++consecutive_failures == RECOVERY_THRESHOLD) {
                ESP_LOGW(TAG, "no radar frames in %dx — re-sending mode-change",
                         RECOVERY_THRESHOLD);
                dp_radar_enter_report_mode();
                consecutive_failures = 0;
            }
            vTaskDelay(read_interval);
            continue;
        }
        consecutive_failures = 0;

        bool near = dp_radar_filter_near(&s);

        TickType_t now = xTaskGetTickCount();
        if (last_publish == 0 || (now - last_publish) >= publish_interval) {
            ESP_LOGI(TAG, "presence=%d distance_cm=%u near=%d", s.presence, s.distance_cm,
                     (int)near);
            // CSV trace for grep + plotter (per-berth gate threshold tuning)
            ESP_LOGI(TAG,
                     "RADAR,%u,%d,%u,"
                     "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                     (unsigned)s.ts_ms, (int)s.presence, (unsigned)s.distance_cm, s.gate_energy[0],
                     s.gate_energy[1], s.gate_energy[2], s.gate_energy[3], s.gate_energy[4],
                     s.gate_energy[5], s.gate_energy[6], s.gate_energy[7], s.gate_energy[8],
                     s.gate_energy[9], s.gate_energy[10], s.gate_energy[11], s.gate_energy[12],
                     s.gate_energy[13], s.gate_energy[14], s.gate_energy[15]);

            if (s_mesh_ready) {
                // berth_id field is informational (gateway prefers its
                // unicast->berth lookup); send NODE_ID as bench fallback
                berth_status_t status = to_status(&s, near, node_id, node_id);
                dp_mesh_publish_status(&status);
#if CONFIG_DOCKPULSE_DIAG_ENABLE
                berth_diag_t diag = to_diag(&s, node_id, node_id);
                dp_mesh_publish_diag(&diag);
#endif
            } else {
                ESP_LOGD(TAG, "skip publish — not adopted");
            }
            last_publish = now;
        }

        vTaskDelay(read_interval);
    }
}
