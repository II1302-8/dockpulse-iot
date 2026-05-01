#include "dp_io.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "dp_io";

static volatile dp_led_state_t s_led_state = DP_LED_OFF;
static int s_led_gpio = -1;
static bool s_led_active_low;

static dp_button_long_press_cb_t s_btn_cb;
static void *s_btn_ctx;

static inline void led_drive(bool on)
{
    if (s_led_gpio < 0) {
        return;
    }
    int level = on ? 1 : 0;
    if (s_led_active_low) {
        level = !level;
    }
    gpio_set_level((gpio_num_t)s_led_gpio, level);
}

static void led_task(void *arg)
{
    (void)arg;
    bool on = false;
    while (true) {
        switch (s_led_state) {
        case DP_LED_OFF:
            led_drive(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case DP_LED_OK:
            led_drive(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case DP_LED_IDLE:
            led_drive(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            led_drive(false);
            vTaskDelay(pdMS_TO_TICKS(900));
            break;
        case DP_LED_PROVISIONING:
            // 4Hz — easy to spot at arm's length while scanning QR
            on = !on;
            led_drive(on);
            vTaskDelay(pdMS_TO_TICKS(125));
            break;
        case DP_LED_ERROR:
            led_drive(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_drive(false);
            vTaskDelay(pdMS_TO_TICKS(120));
            led_drive(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_drive(false);
            vTaskDelay(pdMS_TO_TICKS(1720));
            break;
        }
    }
}

esp_err_t dp_led_init(void)
{
    s_led_gpio = CONFIG_DOCKPULSE_LED_GPIO;
    s_led_active_low = CONFIG_DOCKPULSE_LED_ACTIVE_LOW;
    if (s_led_gpio < 0) {
        ESP_LOGI(TAG, "LED disabled");
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    led_drive(false);
    BaseType_t ok = xTaskCreate(led_task, "dp_led", 2048, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void dp_led_set(dp_led_state_t state) { s_led_state = state; }

// Poll instead of ISR — GPIO 9 is also the C3 boot strap; ISR EDGE
// races with whatever the boot ROM left. Poll loop debounces for free.
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
