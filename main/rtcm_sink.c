// rtcm_sink.c — StreamBuffer-backed seam. See rtcm_sink.h.

#include "rtcm_sink.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "rtcm_sink";

// Full-rate MSM7 (GPS+GLO+GAL) @1Hz is only a few KB/s, but bursts + scheduling
// jitter want slack. 16KB decouples a stalled consumer from dropping USB data.
#define RTCM_SINK_BUF_BYTES   (16 * 1024)
// Wake the consumer once at least this many bytes are available (or on timeout).
#define RTCM_SINK_TRIGGER     1

static StreamBufferHandle_t s_buf;
static StreamBufferHandle_t s_caster_buf;

esp_err_t rtcm_sink_init(void)
{
    s_buf = xStreamBufferCreate(RTCM_SINK_BUF_BYTES, RTCM_SINK_TRIGGER);
    s_caster_buf = xStreamBufferCreate(RTCM_SINK_BUF_BYTES, RTCM_SINK_TRIGGER);
    if (s_buf == NULL || s_caster_buf == NULL) {
        ESP_LOGE(TAG, "xStreamBufferCreate(%d) failed (no mem)", RTCM_SINK_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sink ready (%d byte buffer x2: sink + caster tee)", RTCM_SINK_BUF_BYTES);
    return ESP_OK;
}

size_t rtcm_sink_push(const uint8_t *data, size_t len)
{
    if (s_buf == NULL || data == NULL || len == 0) {
        return 0;
    }
    // Non-blocking (0 tick timeout): the CDC data callback must not block the
    // USB driver task. If this ever short-reads, the consumer is too slow.
    size_t sent = xStreamBufferSend(s_buf, data, len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "sink overflow: dropped %u/%u bytes", (unsigned)(len - sent), (unsigned)len);
    }
    return sent;
}

size_t rtcm_sink_read(uint8_t *out, size_t max_len, TickType_t timeout)
{
    if (s_buf == NULL || out == NULL || max_len == 0) {
        return 0;
    }
    return xStreamBufferReceive(s_buf, out, max_len, timeout);
}

size_t rtcm_caster_push(const uint8_t *data, size_t len)
{
    if (s_caster_buf == NULL || data == NULL || len == 0) {
        return 0;
    }
    // Non-blocking: the drain task must keep servicing the primary sink. Bytes
    // are silently dropped until the caster starts draining s_caster_buf — that
    // is fine, the caster only needs the live stream once it is running.
    return xStreamBufferSend(s_caster_buf, data, len, 0);
}

size_t rtcm_caster_read(uint8_t *out, size_t max_len, TickType_t timeout)
{
    if (s_caster_buf == NULL || out == NULL || max_len == 0) {
        return 0;
    }
    return xStreamBufferReceive(s_caster_buf, out, max_len, timeout);
}
