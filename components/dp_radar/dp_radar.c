// Driver for Waveshare HMMD mmWave Sensor (S3KM1110, 24 GHz FMCW).
// UART, 115200 8N1, little-endian.
//
// The module supports three output modes (Normal / Debug / Report). We
// switch it to Report mode at init so it streams binary frames every
// ~100 ms with header F4 F3 F2 F1 and tail F8 F7 F6 F5. That gives us
// presence (1 byte) + target distance (2 bytes, cm) + 16 energy gates
// (32 bytes) per frame — see the Waveshare wiki, "Communication
// Protocol → Module Report Data → Report Mode".
//
// Mode-change uses the Command frame format (header FD FC FB FA, tail
// 04 03 02 01) with command 0x0012 / value 0x00000004

#include "dp_radar.h"

#include "sdkconfig.h"

#if CONFIG_DOCKPULSE_ROLE_SENSOR && !CONFIG_DOCKPULSE_RADAR_FAKE

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define RADAR_PORT CONFIG_DOCKPULSE_RADAR_UART_PORT
#define RADAR_TX   CONFIG_DOCKPULSE_RADAR_UART_TX
#define RADAR_RX   CONFIG_DOCKPULSE_RADAR_UART_RX
#define RADAR_BAUD CONFIG_DOCKPULSE_RADAR_UART_BAUD

#define RX_BUF_SZ 1024
#define MAX_FRAME 64 // report frame is ~43 bytes

static const char *TAG = "dp_radar";

static const uint8_t REPORT_HDR[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t REPORT_TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

static const uint8_t CMD_HDR[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t CMD_TAIL[4] = {0x04, 0x03, 0x02, 0x01};

// Command: switch to Report mode.
//   header(4) | len(2)=0x0008 | cmd(2)=0x0012 | param_id(2)=0x0000 |
//   value(4)=0x00000004 | tail(4)
static const uint8_t CMD_ENTER_REPORT_MODE[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x12, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01,
};

// Read one command-frame ACK and verify it matches `req_cmd`. Per the
// HMMD protocol, responses use the same FD FC FB FA / 04 03 02 01
// framing as requests, with body[0..1] = req_cmd | 0x0100 (response
// bit). We don't validate the rest of the body — a matching cmd word
// is enough to confirm the module saw and accepted the request
static esp_err_t read_command_ack(uint16_t req_cmd, TickType_t timeout)
{
    enum { SYNC_HDR, READ_LEN, READ_BODY } state = SYNC_HDR;
    uint8_t hdr_match = 0;
    uint8_t frame[MAX_FRAME];
    size_t frame_len = 0;
    uint16_t expected_body = 0;
    const uint16_t want_cmd = req_cmd | 0x0100;

    const TickType_t deadline = xTaskGetTickCount() + timeout;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        uint8_t b;
        int n = uart_read_bytes(RADAR_PORT, &b, 1, deadline - now);
        if (n <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        switch (state) {
        case SYNC_HDR:
            if (b == CMD_HDR[hdr_match]) {
                hdr_match++;
                if (hdr_match == sizeof(CMD_HDR)) {
                    memcpy(frame, CMD_HDR, sizeof(CMD_HDR));
                    frame_len = sizeof(CMD_HDR);
                    state = READ_LEN;
                }
            } else {
                hdr_match = (b == CMD_HDR[0]) ? 1 : 0;
            }
            break;

        case READ_LEN:
            frame[frame_len++] = b;
            if (frame_len == sizeof(CMD_HDR) + 2) {
                expected_body = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
                if (expected_body < 2 ||
                    expected_body + sizeof(CMD_HDR) + 2 + sizeof(CMD_TAIL) > MAX_FRAME) {
                    return ESP_FAIL;
                }
                state = READ_BODY;
            }
            break;

        case READ_BODY:
            frame[frame_len++] = b;
            if (frame_len == sizeof(CMD_HDR) + 2 + expected_body + sizeof(CMD_TAIL)) {
                const uint8_t *tail = &frame[frame_len - sizeof(CMD_TAIL)];
                if (memcmp(tail, CMD_TAIL, sizeof(CMD_TAIL)) != 0) {
                    return ESP_FAIL;
                }
                const uint8_t *body = &frame[sizeof(CMD_HDR) + 2];
                uint16_t got_cmd = (uint16_t)body[0] | ((uint16_t)body[1] << 8);
                if (got_cmd != want_cmd) {
                    return ESP_FAIL;
                }
                return ESP_OK;
            }
            break;
        }
    }
}

// Send the mode-change command. Idempotent — safe to call repeatedly
// for recovery when the module appears to have stopped streaming.
// Logs ACK status. Flushes any data the module dribbled back
esp_err_t dp_radar_enter_report_mode(void)
{
    int written =
        uart_write_bytes(RADAR_PORT, CMD_ENTER_REPORT_MODE, sizeof(CMD_ENTER_REPORT_MODE));
    if (written != (int)sizeof(CMD_ENTER_REPORT_MODE)) {
        ESP_LOGE(TAG, "mode-change write failed (%d/%u)", written,
                 (unsigned)sizeof(CMD_ENTER_REPORT_MODE));
        return ESP_FAIL;
    }
    uart_wait_tx_done(RADAR_PORT, pdMS_TO_TICKS(100));

    esp_err_t ack = read_command_ack(0x0012, pdMS_TO_TICKS(500));
    if (ack != ESP_OK) {
        ESP_LOGW(TAG, "mode-change ACK not seen (%s)", esp_err_to_name(ack));
    } else {
        ESP_LOGI(TAG, "mode-change ACK ok (Report mode)");
    }
    uart_flush_input(RADAR_PORT);
    return ack;
}

esp_err_t dp_radar_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = RADAR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RADAR_PORT, RX_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RADAR_PORT, &cfg));
    ESP_ERROR_CHECK(
        uart_set_pin(RADAR_PORT, RADAR_TX, RADAR_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "uart%d tx=%d rx=%d baud=%d", RADAR_PORT, RADAR_TX, RADAR_RX, RADAR_BAUD);

    // Best-effort. Failure here is most often a wiring/baud problem,
    // which dp_radar_read() will surface as repeated timeouts and the
    // sensor loop will recover from by re-sending the command
    dp_radar_enter_report_mode();
    return ESP_OK;
}

// Read until we have a full report frame (header...tail, length-validated).
// Returns ESP_OK on a valid frame, ESP_ERR_TIMEOUT if `timeout` elapsed
esp_err_t dp_radar_read(dp_radar_sample_t *out, TickType_t timeout)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;

    // The HMMD streams ~14 KB/s in Report mode (~10 frames × ~45 B each
    // every 100 ms). Our 2 KB RX FIFO overflows within ~140 ms, so by
    // the time the sensor task wakes from its multi-second period the
    // buffer holds stale, mid-frame garbage from an indeterminate
    // overflow point. Flushing here forces the parser to re-sync
    // against the next freshly-received frame instead of chewing
    // through KB of out-of-band bytes hunting for a header
    uart_flush_input(RADAR_PORT);

    enum { SYNC_HDR, READ_LEN, READ_BODY } state = SYNC_HDR;
    uint8_t frame[MAX_FRAME];
    size_t frame_len = 0;
    uint8_t hdr_match = 0;
    uint16_t expected_body = 0;

    const TickType_t deadline = xTaskGetTickCount() + timeout;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0)
            return ESP_ERR_TIMEOUT;

        uint8_t b;
        int n = uart_read_bytes(RADAR_PORT, &b, 1, deadline - now);
        if (n <= 0)
            return ESP_ERR_TIMEOUT;

        switch (state) {
        case SYNC_HDR:
            if (b == REPORT_HDR[hdr_match]) {
                hdr_match++;
                if (hdr_match == sizeof(REPORT_HDR)) {
                    memcpy(frame, REPORT_HDR, sizeof(REPORT_HDR));
                    frame_len = sizeof(REPORT_HDR);
                    state = READ_LEN;
                }
            } else {
                hdr_match = (b == REPORT_HDR[0]) ? 1 : 0;
            }
            break;

        case READ_LEN:
            frame[frame_len++] = b;
            if (frame_len == sizeof(REPORT_HDR) + 2) {
                expected_body = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
                // need state(1)+dist(2); shorter aliases tail
                if (expected_body < 3 ||
                    expected_body + sizeof(REPORT_HDR) + 2 + sizeof(REPORT_TAIL) > MAX_FRAME) {
                    ESP_LOGW(TAG, "bad frame body=%u, resync", expected_body);
                    state = SYNC_HDR;
                    hdr_match = 0;
                    frame_len = 0;
                    break;
                }
                state = READ_BODY;
            }
            break;

        case READ_BODY:
            frame[frame_len++] = b;
            if (frame_len == sizeof(REPORT_HDR) + 2 + expected_body + sizeof(REPORT_TAIL)) {
                const uint8_t *tail = &frame[frame_len - sizeof(REPORT_TAIL)];
                if (memcmp(tail, REPORT_TAIL, sizeof(REPORT_TAIL)) != 0) {
                    ESP_LOGW(TAG, "tail mismatch, resync");
                    state = SYNC_HDR;
                    hdr_match = 0;
                    frame_len = 0;
                    break;
                }
                // body[0]     state: 0 none, 1 moving, 2 static, 3 both
                // body[1..2]  distance cm LE
                // body[3..34] 16 gate energies LE u16
                const uint8_t *body = &frame[sizeof(REPORT_HDR) + 2];
                out->target_state = (int8_t)body[0];
                out->presence = out->target_state != 0;
                out->distance_cm = (uint16_t)body[1] | ((uint16_t)body[2] << 8);
                out->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);

                if (expected_body >= 3 + 2 * DP_RADAR_GATE_COUNT) {
                    const uint8_t *e = &body[3];
                    for (size_t i = 0; i < DP_RADAR_GATE_COUNT; i++) {
                        out->gate_energy[i] = (uint16_t)e[2 * i] | ((uint16_t)e[2 * i + 1] << 8);
                    }
                } else {
                    memset(out->gate_energy, 0, sizeof(out->gate_energy));
                }
                return ESP_OK;
            }
            break;
        }
    }
}

#elif CONFIG_DOCKPULSE_ROLE_SENSOR && CONFIG_DOCKPULSE_RADAR_FAKE

// Fake radar for bench-testing the mesh + gateway path with no
// hardware attached. Walks the distance through a 30-step cycle so
// the gateway sees a recognisable pattern, with one synthetic peak in
// the gate matching the current distance

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dp_radar_fake";
static uint32_t s_counter;

esp_err_t dp_radar_init(void)
{
    ESP_LOGW(TAG, "FAKE radar — synthetic samples, no UART");
    s_counter = 0;
    return ESP_OK;
}

esp_err_t dp_radar_read(dp_radar_sample_t *out, TickType_t timeout)
{
    (void)timeout;
    if (!out)
        return ESP_ERR_INVALID_ARG;

    uint32_t step = s_counter++ % 30;
    uint16_t distance_cm = (uint16_t)(200 + step * 20); // 200..780 cm
    out->presence = distance_cm > 100;
    out->distance_cm = distance_cm;
    out->target_state = 1;
    out->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);

    memset(out->gate_energy, 0, sizeof(out->gate_energy));
    size_t gate = (size_t)(distance_cm / DP_RADAR_GATE_CM);
    if (gate < DP_RADAR_GATE_COUNT) {
        out->gate_energy[gate] = 0x4000;
    }
    return ESP_OK;
}

esp_err_t dp_radar_enter_report_mode(void) { return ESP_OK; }

#else // !CONFIG_DOCKPULSE_ROLE_SENSOR — stub out for gateway build

esp_err_t dp_radar_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t dp_radar_read(dp_radar_sample_t *out, TickType_t timeout)
{
    (void)out;
    (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t dp_radar_enter_report_mode(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
