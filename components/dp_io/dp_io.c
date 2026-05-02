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
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, g, r, b);
    led_strip_refresh(s_strip);
}
#define LED_OFF()         led_rgb(0, 0, 0)
#define LED_OK_COLOR()    led_rgb(0, 12, 0)     // dim green
#define LED_PROV_COLOR()  led_rgb(0, 0, 24)     // medium blue
#define LED_ERR_COLOR()   led_rgb(24, 0, 0)     // red
#else
static inline void led_drive(bool on)
{
    if (s_led_gpio < 0) return;
    int level = on ? 1 : 0;
    if (s_led_active_low) level = !level;
    gpio_set_level((gpio_num_t)s_led_gpio, level);
}
#define LED_OFF()         led_drive(false)
#define LED_OK_COLOR()    led_drive(true)
#define LED_PROV_COLOR()  led_drive(true)
#define LED_ERR_COLOR()   led_drive(true)
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
            if (on) LED_PROV_COLOR(); else LED_OFF();
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
    if (err != ESP_OK) return err;
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
