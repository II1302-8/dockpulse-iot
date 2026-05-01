#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

#if CONFIG_DOCKPULSE_MQTT_TLS
#include "esp_crt_bundle.h"
extern const char client_crt_start[] asm("_binary_client_crt_start");
extern const char client_key_start[] asm("_binary_client_key_start");
#endif

static const char *TAG = "dp_gw_mqtt";

#define MQTT_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_mqtt_events;

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "broker connected");
        xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "broker disconnected");
        xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGE(TAG, "broker error: type=%d tls_err=0x%x tls_stack_err=0x%x sock_errno=%d",
                     event->error_handle->error_type, event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "broker error");
        }
        break;
    default:
        break;
    }
}

esp_err_t dp_gateway_mqtt_start_and_wait(void)
{
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_DOCKPULSE_MQTT_BROKER_URI,
    };
    if (CONFIG_DOCKPULSE_MQTT_CLIENT_ID[0] != '\0') {
        cfg.credentials.client_id = CONFIG_DOCKPULSE_MQTT_CLIENT_ID;
    }

#if CONFIG_DOCKPULSE_MQTT_TLS
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.credentials.authentication.certificate = client_crt_start;
    cfg.credentials.authentication.key = client_key_start;
#endif

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, &on_mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_events, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(15000));
    if (!(bits & MQTT_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "broker connect timeout (uri=%s)", CONFIG_DOCKPULSE_MQTT_BROKER_URI);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t dp_gateway_mqtt_publish(const char *topic, const char *payload, int qos)
{
    if (!s_client || !topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
    if (!(bits & MQTT_CONNECTED_BIT)) {
        // log every 16th drop so a lost broker doesn't spam
        static uint32_t s_dropped;
        if ((s_dropped++ & 0x0F) == 0) {
            ESP_LOGW(TAG, "drop publish, broker disconnected (total=%" PRIu32 ")", s_dropped);
        }
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

#endif
