// app_main.c — tab5-caster entry point.
//
// Milestone-1 wiring: bring up the USB CDC source and drain rtcm_sink into a
// hex-dump / RTCM3-preamble counter. This proves "plug in Mosaic -> bytes with
// 0xD3 frames arrive" end to end, with ZERO caster code involved yet.
//
// Later: replace rtcm_dump_task with the Zig ntripcaster embedded build —
// register a local Source and feed src.ring from rtcm_sink_read() (io.Stream
// backed by the sink on the lwip/embedded target).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "rtcm_sink.h"
#include "usb_cdc_source.h"

static const char *TAG = "tab5-caster";

// Milestone-1 consumer: count bytes + RTCM3 preambles (0xD3), log periodically.
// A healthy Mosaic feed shows a steady stream of 0xD3-led frames.
static void rtcm_dump_task(void *arg)
{
    uint8_t buf[512];
    uint32_t total_bytes = 0;
    uint32_t preambles = 0;
    TickType_t last_log = xTaskGetTickCount();

    while (1) {
        size_t n = rtcm_sink_read(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == 0xD3) preambles++;   // RTCM3 frame preamble
        }
        total_bytes += n;

        // Once per ~5s, report throughput + preamble count as a liveness check.
        if (xTaskGetTickCount() - last_log > pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "rx: %lu bytes, %lu RTCM3 preambles (0xD3)",
                     (unsigned long)total_bytes, (unsigned long)preambles);
            if (n) {
                ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n < 32 ? n : 32, ESP_LOG_DEBUG);
            }
            last_log = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "tab5-caster boot (ESP32-P4 / Mosaic USB CDC -> caster)");

    ESP_ERROR_CHECK(rtcm_sink_init());
    ESP_ERROR_CHECK(usb_cdc_source_start());

    xTaskCreate(rtcm_dump_task, "rtcm_dump", 4096, NULL, 4, NULL);
}
