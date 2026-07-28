// app_main.c — tab5-caster entry point.
//
// Milestone-1 wiring: USB CDC source -> rtcm_sink -> monitor task feeds
// rtcm_monitor (CRC-validated frame tally). A debug console lets you inspect
// what the Mosaic is actually sending — all with ZERO caster code yet.
//
// Later: replace rtcm_feed_task with the Zig ntripcaster embedded build —
// register a local Source and feed src.ring from rtcm_sink_read() (io.Stream
// backed by the sink on the lwip/embedded target). rtcm_monitor can stay as a
// permanent bring-up/diagnostic tap.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "rtcm_sink.h"
#include "rtcm_monitor.h"
#include "usb_cdc_source.h"
#include "debug_console.h"

static const char *TAG = "tab5-caster";

// Drains the sink and feeds the monitor. Also logs a periodic liveness line so
// `idf.py monitor` shows activity without needing to type console commands.
static void rtcm_feed_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    TickType_t last_log = xTaskGetTickCount();

    while (1) {
        size_t n = rtcm_sink_read(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (n) {
            rtcm_monitor_feed(buf, n);
        }
        if (xTaskGetTickCount() - last_log > pdMS_TO_TICKS(5000)) {
            rtcm_mon_stats_t s;
            rtcm_monitor_get(&s);
            ESP_LOGI(TAG, "rx: %llu bytes, %llu valid RTCM3 frames, %llu CRC fails",
                     (unsigned long long)s.total_bytes,
                     (unsigned long long)s.valid_frames,
                     (unsigned long long)s.crc_fails);
            last_log = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "tab5-caster boot (ESP32-P4 / Mosaic USB CDC -> caster)");

    rtcm_monitor_init();
    ESP_ERROR_CHECK(rtcm_sink_init());
    ESP_ERROR_CHECK(usb_cdc_source_start());

    xTaskCreate(rtcm_feed_task, "rtcm_feed", 4096, NULL, 4, NULL);

    debug_console_start();
}
