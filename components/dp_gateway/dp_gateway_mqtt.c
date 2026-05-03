#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_GATEWAY && !CONFIG_DOCKPULSE_GATEWAY_UPLINK_STUB

#include "dp_gateway_priv.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#if CONFIG_DOCKPULSE_MQTT_TLS
#include "esp_crt_bundle.h"
extern const char client_crt_start[] asm("_binary_client_crt_start");
extern const char client_key_start[] asm("_binary_client_key_start");
#endif

static const char *TAG = "dp_gw_mqtt";

#define MQTT_CONNECTED_BIT  BIT0
#define DP_MAX_PENDING_SUBS 4
#define DP_MAX_MSG_CBS      4

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_mqtt_events;

// rx fans out, each subsystem registers and filters by topic itself
// count atomic for lock-free read on event task, append from app side
static dp_gateway_mqtt_msg_cb_t s_msg_cbs[DP_MAX_MSG_CBS];
static atomic_int s_msg_cb_count;

// queued subs pre-connect. s_subs_lock guards app + event task both touch
typedef struct {
    char topic[96];
    int qos;
    bool active;
} pending_sub_t;
static pending_sub_t s_pending_subs[DP_MAX_PENDING_SUBS];
static SemaphoreHandle_t s_subs_lock;

static void apply_pending_subs(void)
{
    if (s_subs_lock) {
        xSemaphoreTake(s_subs_lock, portMAX_DELAY);
    }
    for (int i = 0; i < DP_MAX_PENDING_SUBS; i++) {
        if (s_pending_subs[i].active) {
            int mid =
                esp_mqtt_client_subscribe(s_client, s_pending_subs[i].topic, s_pending_subs[i].qos);
            ESP_LOGI(TAG, "subscribe %s qos=%d mid=%d", s_pending_subs[i].topic,
                     s_pending_subs[i].qos, mid);
        }
    }
    if (s_subs_lock) {
        xSemaphoreGive(s_subs_lock);
    }
}

// LWT topic + payload live for client lifetime. esp_mqtt_client stores by ref
static char s_lwt_topic[160];
static const char LWT_PAYLOAD[] = "{\"online\":false}";

static void publish_online_retained(void)
{
    if (s_lwt_topic[0]) {
        esp_mqtt_client_publish(s_client, s_lwt_topic, "{\"online\":true}", 0, 1, 1);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "broker connected");
        xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        apply_pending_subs();
        publish_online_retained();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "broker disconnected");
        xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DATA: {
        if (!event || !event->topic || !event->data) {
            break;
        }
        // event->topic is NOT null-terminated, copy out
        char topic[160];
        int tlen =
            event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, tlen);
        topic[tlen] = '\0';
        int n = atomic_load(&s_msg_cb_count);
        for (int i = 0; i < n; i++) {
            s_msg_cbs[i](topic, event->data, event->data_len);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGE(TAG, "broker error: type=%d tls=0x%x stack=0x%x sock=%d",
                     event->error_handle->error_type, event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

esp_err_t dp_gateway_mqtt_add_msg_cb(dp_gateway_mqtt_msg_cb_t cb)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = atomic_load(&s_msg_cb_count);
    if (n >= DP_MAX_MSG_CBS) {
        return ESP_ERR_NO_MEM;
    }
    s_msg_cbs[n] = cb;
    atomic_store(&s_msg_cb_count, n + 1);
    return ESP_OK;
}

esp_err_t dp_gateway_mqtt_subscribe(const char *topic_filter, int qos)
{
    if (!topic_filter) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_subs_lock) {
        xSemaphoreTake(s_subs_lock, portMAX_DELAY);
    }
    esp_err_t ret = ESP_ERR_NO_MEM;
    for (int i = 0; i < DP_MAX_PENDING_SUBS; i++) {
        if (!s_pending_subs[i].active) {
            strncpy(s_pending_subs[i].topic, topic_filter, sizeof(s_pending_subs[i].topic) - 1);
            s_pending_subs[i].qos = qos;
            s_pending_subs[i].active = true;
            ret = ESP_OK;
            // if already connected, register now too
            if (s_client && s_mqtt_events &&
                (xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT)) {
                int mid = esp_mqtt_client_subscribe(s_client, topic_filter, qos);
                ESP_LOGI(TAG, "subscribe %s qos=%d mid=%d", topic_filter, qos, mid);
            }
            break;
        }
    }
    if (s_subs_lock) {
        xSemaphoreGive(s_subs_lock);
    }
    return ret;
}

esp_err_t dp_gateway_mqtt_start_and_wait(void)
{
    s_mqtt_events = xEventGroupCreate();
    if (!s_mqtt_events) {
        return ESP_ERR_NO_MEM;
    }
    // before mqtt_start so event-task apply_pending_subs sees it
    s_subs_lock = xSemaphoreCreateMutex();
    if (!s_subs_lock) {
        return ESP_ERR_NO_MEM;
    }

    // LWT topic uses the configured gateway id
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "dockpulse/v1/gw/%s/status",
             CONFIG_DOCKPULSE_GATEWAY_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_DOCKPULSE_MQTT_BROKER_URI,
        .session.last_will.topic = s_lwt_topic,
        .session.last_will.msg = LWT_PAYLOAD,
        .session.last_will.msg_len = (int)(sizeof(LWT_PAYLOAD) - 1),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
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

static esp_err_t publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
    if (!(bits & MQTT_CONNECTED_BIT)) {
        static uint32_t s_dropped;
        if ((s_dropped++ & 0x0F) == 0) {
            ESP_LOGW(TAG, "drop publish, broker disconnected (total=%" PRIu32 ")", s_dropped);
        }
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t dp_gateway_mqtt_publish(const char *topic, const char *payload, int qos)
{
    return publish(topic, payload, qos, false);
}

esp_err_t dp_gateway_mqtt_publish_retained(const char *topic, const char *payload, int qos)
{
    return publish(topic, payload, qos, true);
}

#endif
