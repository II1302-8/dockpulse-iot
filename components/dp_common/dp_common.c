#include "dp_common.h"

#include "sdkconfig.h"

esp_err_t dp_common_get_node_id(uint8_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (uint8_t)CONFIG_DOCKPULSE_NODE_ID;
    return ESP_OK;
}

esp_err_t berth_status_pack(const berth_status_t *s, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!s || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cap < BERTH_STATUS_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = s->node_id;
    buf[1] = (uint8_t)(s->berth_id & 0xFF);
    buf[2] = (uint8_t)(s->berth_id >> 8);
    buf[3] = s->occupied ? 1 : 0;
    buf[4] = (uint8_t)(s->sensor_raw_mm & 0xFF);
    buf[5] = (uint8_t)(s->sensor_raw_mm >> 8);
    buf[6] = s->battery_pct;
    buf[7] = (uint8_t)(s->ts_ms & 0xFF);
    buf[8] = (uint8_t)((s->ts_ms >> 8) & 0xFF);
    buf[9] = (uint8_t)((s->ts_ms >> 16) & 0xFF);
    buf[10] = (uint8_t)((s->ts_ms >> 24) & 0xFF);
    if (out_len) {
        *out_len = BERTH_STATUS_WIRE_LEN;
    }
    return ESP_OK;
}

esp_err_t berth_status_unpack(const uint8_t *buf, size_t len, berth_status_t *out)
{
    if (!buf || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < BERTH_STATUS_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->node_id = buf[0];
    out->berth_id = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
    out->occupied = buf[3] != 0;
    out->sensor_raw_mm = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    out->battery_pct = buf[6];
    out->ts_ms = (uint32_t)buf[7] | ((uint32_t)buf[8] << 8) | ((uint32_t)buf[9] << 16) |
                 ((uint32_t)buf[10] << 24);
    return ESP_OK;
}

esp_err_t berth_diag_pack(const berth_diag_t *d, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!d || !buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cap < BERTH_DIAG_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t i = 0;
    buf[i++] = d->node_id;
    buf[i++] = (uint8_t)(d->berth_id & 0xFF);
    buf[i++] = (uint8_t)(d->berth_id >> 8);
    buf[i++] = (uint8_t)d->target_state;
    buf[i++] = (uint8_t)(d->raw_distance_cm & 0xFF);
    buf[i++] = (uint8_t)(d->raw_distance_cm >> 8);
    for (size_t g = 0; g < DP_RADAR_GATE_COUNT; g++) {
        buf[i++] = (uint8_t)(d->gate_energy[g] & 0xFF);
        buf[i++] = (uint8_t)(d->gate_energy[g] >> 8);
    }
    buf[i++] = (uint8_t)(d->ts_ms & 0xFF);
    buf[i++] = (uint8_t)((d->ts_ms >> 8) & 0xFF);
    buf[i++] = (uint8_t)((d->ts_ms >> 16) & 0xFF);
    buf[i++] = (uint8_t)((d->ts_ms >> 24) & 0xFF);
    if (out_len) {
        *out_len = i;
    }
    return ESP_OK;
}

esp_err_t berth_diag_unpack(const uint8_t *buf, size_t len, berth_diag_t *out)
{
    if (!buf || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < BERTH_DIAG_WIRE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t i = 0;
    out->node_id = buf[i++];
    out->berth_id = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
    i += 2;
    out->target_state = (int8_t)buf[i++];
    out->raw_distance_cm = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
    i += 2;
    for (size_t g = 0; g < DP_RADAR_GATE_COUNT; g++) {
        out->gate_energy[g] = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
        i += 2;
    }
    out->ts_ms = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) | ((uint32_t)buf[i + 2] << 16) |
                 ((uint32_t)buf[i + 3] << 24);
    return ESP_OK;
}
