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
#include "esp_event.h"
#include "esp_netif.h"

#include "rtcm_sink.h"
#include "rtcm_monitor.h"
#include "usb_cdc_source.h"
#include "debug_console.h"
#include "net_mdns.h"
#include "board_power.h"
#include "wifi_sta.h"

static const char *TAG = "tab5-caster";

// Service ports the caster will serve on (once the caster + netif land).
#define CASTER_PORT  2101   // NTRIP default
#define ADMIN_PORT   8080   // status / admin web UI

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
            // Tee the same bytes to the caster (no-op until `caster` is started).
            rtcm_caster_push(buf, n);
        }
        if (xTaskGetTickCount() - last_log > pdMS_TO_TICKS(5000)) {
            rtcm_mon_stats_t s;
            rtcm_monitor_get(&s);
            usb_cdc_status_t u;
            usb_cdc_source_status(&u);
            // Fold USB state into the periodic line so the box is diagnosable
            // from any capture window without needing to catch the boot log
            // (the USB-Serial-JTAG console drops one-shot events when unread).
            ESP_LOGI(TAG, "usb[host=%d attach=%d open=%d %04X:%04X n=%u if[%s] "
                     "cur=%d stream=%d] rx: %llu B, %llu RTCM3, %llu CRCfail",
                     u.host_installed, u.device_attached, u.cdc_open, u.vid, u.pid,
                     u.num_interfaces, u.topo, (int8_t)u.cur_itf, (int8_t)u.stream_itf,
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

    // Console FIRST. This is a headless box whose only operator interface is the
    // USB-C serial/JTAG console — the REPL must come up before anything that can
    // block or abort. usb_cdc_source_start() blocks app_main until usb_host_install
    // returns (and aborts on failure); if the HS-OTG PHY/VBUS isn't up on real
    // hardware, starting the console after it would leave the box mute. The REPL
    // runs on its own task, so it survives even if USB host bring-up hangs below.
    rtcm_monitor_init();
    debug_console_start();

    ESP_ERROR_CHECK(rtcm_sink_init());

    // Bring up the TCP/IP stack (tcpip thread + lwIP core) and the default event
    // loop. No actual netif (WiFi/Ethernet) is attached yet — that's TODO(hw) —
    // but this makes lwIP sockets usable, so the caster's listener can bind and
    // mdns can init. Without it, lwip_socket()/mdns_init() have no stack to run on.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(rtcm_feed_task, "rtcm_feed", 4096, NULL, 4, NULL);

    // Bring up the USB->caster core BEFORE WiFi. This box exists to serve RTCM
    // corrections offline, so its core data path (Mosaic USB -> sink -> caster)
    // must never be gated behind the C6 WiFi coprocessor. esp_wifi_init() blocks
    // (retrying the ESP-Hosted SDIO transport) whenever the C6 doesn't come up
    // after reset; that used to hang app_main here before USB ever installed,
    // bricking the caster on a WiFi-only fault. WiFi now runs async + last.

    // Enable USB-A VBUS BEFORE the host installs. On Tab5 the USB-A 5V rail is
    // gated by an I/O expander (PI4IOE #2, P3 = USB5V_EN); without this a device
    // is only trickle-powered and never enumerates on the P4 HS OTG. Non-fatal
    // if it fails — log and press on so the console/USB host still come up for
    // diagnosis (an unpowered port just means "waiting for Mosaic" forever).
    esp_err_t pwr = board_power_init();
    if (pwr != ESP_OK) {
        ESP_LOGE(TAG, "board_power_init failed (%s) — USB-A VBUS may be off",
                 esp_err_to_name(pwr));
    }

    // Install the USB host + CDC-ACM source. Blocks until usb_host_install()
    // returns; on real hardware this succeeds (host=1). Kept ahead of WiFi so a
    // dead C6 can never starve the caster of its byte source.
    ESP_ERROR_CHECK(usb_cdc_source_start());

    // Advertise as rtk.local + _ntrip._tcp for zero-config field discovery.
    // NOTE(hw): needs a netif (WiFi via C6/ESP-Hosted or Ethernet) to be
    // reachable. Starting it now is harmless; it becomes discoverable the moment
    // a netif comes up.
    // TODO(product): per-unit unique hostname (e.g. rtk-<serial>) to avoid
    // rtk.local collisions when several boxes share a field LAN.
    net_mdns_start("rtk", CASTER_PORT, ADMIN_PORT);

    // Join the field WiFi via the onboard C6 (ESP-Hosted/SDIO) — LAST, and on
    // its OWN task (see wifi_sta.c): a flaky C6 costs us WiFi, not the caster.
    // On GOT_IP it logs the address and starts the caster.
    wifi_sta_start();
}
