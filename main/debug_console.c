// debug_console.c — see debug_console.h.

#include "debug_console.h"

#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "rtcm_monitor.h"
#include "usb_cdc_source.h"
#include "caster.h"
#include "wifi_sta.h"

static const char *TAG = "console";

// ── commands ────────────────────────────────────────────────────────────────

static int cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    rtcm_mon_stats_t s;
    rtcm_monitor_get(&s);

    int64_t age_ms = s.last_frame_us ? (esp_timer_get_time() - s.last_frame_us) / 1000 : -1;
    printf("bytes=%llu  valid_frames=%llu  crc_fails=%llu  types=%lu\n",
           (unsigned long long)s.total_bytes,
           (unsigned long long)s.valid_frames,
           (unsigned long long)s.crc_fails,
           (unsigned long)s.type_count);
    if (age_ms < 0) {
        printf("last valid frame: (none yet)\n");
    } else {
        printf("last valid frame: %lld ms ago\n", (long long)age_ms);
    }
    return 0;
}

static int cmd_rtcm(int argc, char **argv)
{
    (void)argc; (void)argv;
    rtcm_mon_stats_t s;
    rtcm_monitor_get(&s);

    if (s.type_count == 0) {
        printf("no RTCM3 frames seen yet\n");
        return 0;
    }
    printf("RTCM3 message types seen:\n");
    for (uint32_t i = 0; i < s.type_count; i++) {
        // Annotate the types the caster / FKP actually care about.
        const char *note = "";
        switch (s.types[i].type) {
        case 1005: case 1006: note = " (station coord)"; break;
        case 1077: note = " (GPS MSM7)";     break;
        case 1087: note = " (GLONASS MSM7)"; break;
        case 1097: note = " (Galileo MSM7)"; break;
        case 1127: note = " (BeiDou MSM7 — caster relays, FKP ignores)"; break;
        case 1019: note = " (GPS ephemeris — needed for FKP)"; break;
        case 1033: note = " (rcv/ant descriptor)"; break;
        case 1230: note = " (GLONASS bias)"; break;
        default: break;
        }
        printf("  %5u : %8lu%s\n", s.types[i].type,
               (unsigned long)s.types[i].count, note);
    }
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    int want = 64;
    if (argc >= 2) {
        want = atoi(argv[1]);
        if (want <= 0) want = 64;
    }
    uint8_t buf[RTCM_MON_SNAP_BYTES];
    size_t n = rtcm_monitor_last_frame(buf, sizeof(buf));
    if (n == 0) {
        printf("no frame captured yet\n");
        return 0;
    }
    if ((size_t)want < n) n = want;
    printf("last frame (%u bytes):\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        printf("%02X ", buf[i]);
        if ((i & 0x0F) == 0x0F) printf("\n");
    }
    if (n & 0x0F) printf("\n");
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t buf[RTCM_MON_RAW_BYTES];
    size_t n = rtcm_monitor_last_raw(buf, sizeof(buf));
    if (n == 0) {
        printf("no bytes seen yet\n");
        return 0;
    }
    printf("last %u raw bytes (hex | ascii):\n", (unsigned)n);
    for (size_t i = 0; i < n; i += 16) {
        printf("%04x  ", (unsigned)i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02X ", buf[i + j]);
            else           printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = buf[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
    return 0;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc; (void)argv;
    usb_cdc_status_t u;
    usb_cdc_source_status(&u);
    printf("host_installed=%d  device_attached=%d  cdc_open=%d\n",
           u.host_installed, u.device_attached, u.cdc_open);
    if (u.device_attached) {
        printf("attached VID=0x%04X PID=0x%04X\n", u.vid, u.pid);
        printf("topology: n=%u if[%s]\n", u.num_interfaces, u.topo);
        printf("cur_itf=%d  stream_itf=%d\n", (int8_t)u.cur_itf, (int8_t)u.stream_itf);
    }
    return 0;
}

static int cmd_caster(int argc, char **argv)
{
    (void)argc; (void)argv;
    // Start the Zig ntripcaster: TCP listener (rovers pull /MOSAIC) + local
    // source feeder draining rtcm_sink. NOTE: it shares rtcm_sink with the
    // monitor's feed task, so bytes are split between them — for a clean caster
    // stream you'd stop the monitor drain first. Also needs a netif to be
    // reachable (TODO(hw)); binding without one is harmless (accept just waits).
    int rc = caster_start();
    if (rc == 0) printf("caster started (listening on :2101, mount /MOSAIC)\n");
    else         printf("caster_start failed: %d\n", rc);
    return 0;
}

static int cmd_csource(int argc, char **argv)
{
    (void)argc; (void)argv;
    caster_source_stats_t s;
    caster_source_stats(&s);
    if (!s.running) {
        printf("caster not started (run 'caster' first)\n");
        return 0;
    }
    if (!s.source_present) {
        printf("caster running, but /MOSAIC source not registered yet\n");
        return 0;
    }
    // These come from the Zig SourceFeeder draining the tee — separate from the
    // C monitor's 'stats'/'rtcm' (which tap the primary sink).
    printf("/MOSAIC: bytes_in=%llu  rtcm_detected=%d  clients=%u  types=%u\n",
           s.bytes_in, s.rtcm_detected, s.client_count, s.num_msg_types);
    for (unsigned i = 0; i < s.num_msg_types && i < 12; i++) {
        printf("  %5u : %lu\n", s.types[i], (unsigned long)s.counts[i]);
    }
    return 0;
}

static int cmd_wifireset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("erasing WiFi credentials and rebooting into setup portal...\n");
    wifi_forget();  // does not return (reboots)
    return 0;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "stats", .help = "RTCM byte/frame counters + staleness", .func = cmd_stats },
        { .command = "rtcm",  .help = "RTCM3 message-type histogram",         .func = cmd_rtcm  },
        { .command = "dump",  .help = "hexdump last valid frame [n bytes]",   .hint = "[n]", .func = cmd_dump },
        { .command = "raw",   .help = "hex+ascii of last raw bytes (any content)", .func = cmd_raw },
        { .command = "usb",   .help = "USB host / CDC attach state",          .func = cmd_usb   },
        { .command = "caster", .help = "start the Zig ntripcaster (listener + local source)", .func = cmd_caster },
        { .command = "csource", .help = "caster's /MOSAIC source state (bytes/types via the tee)", .func = cmd_csource },
        { .command = "wifireset", .help = "erase WiFi creds + reboot into the setup portal", .func = cmd_wifireset },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void debug_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "tab5>";
    repl_config.max_cmdline_length = 128;

    esp_console_register_help_command();
    register_cmds();

    // Pick the REPL transport to match the configured console device so it works
    // whether the Tab5 exposes UART or USB-Serial-JTAG for monitor.
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl));
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_config, &repl));
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "debug console ready (type 'help')");
}
