#include "dp_io.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_LED_WS2812
#include "led_strip.h"
#endif

static const char *TAG = "dp_io";

static volatile dp_led_state_t s_led_state = DP_LED_OFF;
static int s_led_gpio = -1;

#if CONFIG_DOCKPULSE_LED_WS2812
static led_strip_handle_t s_strip;
#else
static bool s_led_active_low;
#endif

static dp_button_long_press_cb_t s_btn_cb;
static void *s_btn_ctx;

#if CONFIG_DOCKPULSE_LED_WS2812
// c3-zero LED is RGB-native led_strip 2.5.x only ships GRB so swap r/g here
static inline void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip)
        return;
    led_strip_set_pixel(s_strip, 0, g, r, b);
    led_strip_refresh(s_strip);
}
#define LED_OFF()        led_rgb(0, 0, 0)
#define LED_OK_COLOR()   led_rgb(0, 12, 0) // dim green
#define LED_PROV_COLOR() led_rgb(0, 0, 24) // medium blue
#define LED_ERR_COLOR()  led_rgb(24, 0, 0) // red
#else
static inline void led_drive(bool on)
{
    if (s_led_gpio < 0)
        return;
    int level = on ? 1 : 0;
    if (s_led_active_low)
        level = !level;
    gpio_set_level((gpio_num_t)s_led_gpio, level);
}
#define LED_OFF()        led_drive(false)
#define LED_OK_COLOR()   led_drive(true)
#define LED_PROV_COLOR() led_drive(true)
#define LED_ERR_COLOR()  led_drive(true)
#endif

static void led_task(void *arg)
{
    (void)arg;
    bool on = false;
    while (true) {
        switch (s_led_state) {
        case DP_LED_OFF:
            LED_OFF();
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case DP_LED_OK:
            LED_OK_COLOR();
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case DP_LED_IDLE:
            LED_OK_COLOR();
            vTaskDelay(pdMS_TO_TICKS(100));
            LED_OFF();
            vTaskDelay(pdMS_TO_TICKS(900));
            break;
        case DP_LED_PROVISIONING:
            // 4Hz easy to spot scanning QR
            on = !on;
            if (on)
                LED_PROV_COLOR();
            else
                LED_OFF();
            vTaskDelay(pdMS_TO_TICKS(125));
            break;
        case DP_LED_ERROR:
            LED_ERR_COLOR();
            vTaskDelay(pdMS_TO_TICKS(80));
            LED_OFF();
            vTaskDelay(pdMS_TO_TICKS(120));
            LED_ERR_COLOR();
            vTaskDelay(pdMS_TO_TICKS(80));
            LED_OFF();
            vTaskDelay(pdMS_TO_TICKS(1720));
            break;
        }
    }
}

esp_err_t dp_led_init(void)
{
    s_led_gpio = CONFIG_DOCKPULSE_LED_GPIO;
    if (s_led_gpio < 0) {
        ESP_LOGI(TAG, "LED disabled");
        return ESP_OK;
    }

#if CONFIG_DOCKPULSE_LED_WS2812
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = s_led_gpio,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = false},
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip new err=%d", err);
        return err;
    }
    led_strip_clear(s_strip);
#else
    s_led_active_low = CONFIG_DOCKPULSE_LED_ACTIVE_LOW;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
        return err;
    led_drive(false);
#endif

    BaseType_t ok = xTaskCreate(led_task, "dp_led", 2560, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void dp_led_set(dp_led_state_t state) { s_led_state = state; }

// poll not ISR. gpio 9 is c3 boot strap so ISR edge races boot ROM.
// poll loop debounces for free
static void button_task(void *arg)
{
    (void)arg;
    const int gpio = CONFIG_DOCKPULSE_FACTORY_RESET_GPIO;
    const int hold_ms = CONFIG_DOCKPULSE_FACTORY_RESET_HOLD_MS;
    const TickType_t poll = pdMS_TO_TICKS(50);
    int held_ms = 0;
    while (true) {
        if (gpio_get_level((gpio_num_t)gpio) == 0) {
            held_ms += 50;
            if (held_ms >= hold_ms) {
                ESP_LOGW(TAG, "factory-reset held %dms — firing cb", held_ms);
                if (s_btn_cb) {
                    s_btn_cb(s_btn_ctx);
                }
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(poll);
    }
}

esp_err_t dp_button_init(dp_button_long_press_cb_t cb, void *user_ctx)
{
    s_btn_cb = cb;
    s_btn_ctx = user_ctx;
    int gpio = CONFIG_DOCKPULSE_FACTORY_RESET_GPIO;
    if (gpio < 0) {
        ESP_LOGI(TAG, "factory-reset disabled");
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t ok = xTaskCreate(button_task, "dp_btn", 2048, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}


// ── ADDED: Battery ADC ────────────────────────────────────────────────────────
// Reads battery+ voltage through a 1:1 divider (R1=R2=100 kΩ) on
// CONFIG_DOCKPULSE_BATTERY_ADC_GPIO. Uses the ESP-IDF oneshot ADC driver with
// curve-fitting calibration for linearisation. Falls back to a plain linear
// scale if eFuse calibration data is absent from this chip.

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "dp_common.h" // DP_BATTERY_UNKNOWN

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t         s_cali_handle;
static bool                      s_cali_ok;

esp_err_t dp_battery_init(void)
{
    int gpio = CONFIG_DOCKPULSE_BATTERY_ADC_GPIO;
    if (gpio < 0) {
        ESP_LOGI(TAG, "battery ADC disabled");
        return ESP_OK;
    }

    // Map the GPIO number to an ADC1 channel. On the ESP32-C3:
    //   GPIO 0 = CH0, GPIO 1 = CH1, GPIO 2 = CH2, GPIO 3 = CH3, GPIO 4 = CH4
    // ADC2 must not be used while BLE / Wi-Fi is active.
    adc_channel_t channel = (adc_channel_t)gpio; // CH index == GPIO on C3

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit err=%d", err);
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12, // 0–3.3 V range; divider keeps pin ≤ 2.1 V
        .bitwidth = ADC_BITWIDTH_12, // 0–4095
    };
    err = adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel err=%d", err);
        return err;
    }

    // Curve-fitting calibration gives the best linearity across the range.
    // Most retail ESP32-C3 modules have calibration data burned in eFuse;
    // if not, we fall back to a plain linear scale below.
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = channel,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC calibration unavailable — falling back to linear scale");
    }

    ESP_LOGI(TAG, "battery ADC ready gpio=%d cali=%d", gpio, s_cali_ok);
    return ESP_OK;
}

uint8_t dp_battery_read_pct(void)
{
    if (!s_adc_handle) {
        return DP_BATTERY_UNKNOWN;
    }

    int gpio = CONFIG_DOCKPULSE_BATTERY_ADC_GPIO;
    if (gpio < 0) {
        return DP_BATTERY_UNKNOWN;
    }

    adc_channel_t channel = (adc_channel_t)gpio;

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, channel, &raw) != ESP_OK) {
        return DP_BATTERY_UNKNOWN;
    }

    int v_pin_mv = 0;
    if (s_cali_ok) {
        // Calibrated path: returns millivolts linearised against eFuse data
        adc_cali_raw_to_voltage(s_cali_handle, raw, &v_pin_mv);
    } else {
        // Fallback: simple linear scale (less accurate near rail voltages)
        v_pin_mv = raw * 3300 / 4095;
    }

    // Undo the 1:1 voltage divider to get the actual battery terminal voltage
    int v_batt_mv = v_pin_mv * 2;

    // Li-ion 18650 discharge curve: 3000 mV = empty (0%), 4200 mV = full (100%)
    int pct = (v_batt_mv - 3000) * 100 / (4200 - 3000);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    return (uint8_t)pct;
}
// ── END ADDED ────────────────────────────────────────────────────────────────
