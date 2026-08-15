// debug_console.c — see debug_console.h.

#include "debug_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "rtcm_monitor.h"
#include "usb_cdc_source.h"
#include "caster.h"
#include "upstream.h"
#include "wifi_sta.h"
#include "display.h"
#include "backlight.h"
#include "touch.h"
#include "board_power.h"
#include "driver/i2c_master.h"

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
        case 1107: note = " (SBAS MSM7)";    break;
        case 1117: note = " (QZSS MSM7)";     break;
        case 1127: note = " (BeiDou MSM7 — caster relays, FKP ignores)"; break;
        case 1137: note = " (NavIC MSM7)";    break;
        case 1019: note = " (GPS ephemeris — needed for FKP)";      break;
        case 1020: note = " (GLONASS ephemeris — needed for FKP)";  break;
        case 1042: note = " (BeiDou ephemeris)";                    break;
        case 1044: note = " (QZSS ephemeris)";                      break;
        case 1045: note = " (Galileo F/NAV ephemeris)";            break;
        case 1046: note = " (Galileo I/NAV ephemeris — needed for FKP)"; break;
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

static int cmd_mosaic(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mosaic <septentrio command>\n");
        printf("  e.g. mosaic \\r\\n          (bare prompt -> reveals the port Cd)\n");
        printf("       mosaic setRTCMv3Output, USB1, MSM7+RTCM1006+RTCM1019\n");
        return 0;
    }
    // Rejoin argv[1..] with single spaces — the console splits on whitespace,
    // but Septentrio commands carry spaces (e.g. "setRTCMv3Output, USB1, ...").
    char cmd[224];
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(cmd) - 1; i++) {
        int w = snprintf(cmd + pos, sizeof(cmd) - pos, "%s%s",
                         i > 1 ? " " : "", argv[i]);
        if (w > 0) pos += (size_t)w;
    }

    char reply[1024];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 1500);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("no Mosaic CDC interface open — check `usb`\n");
        return 0;
    }
    if (err != ESP_OK) {
        printf("command TX failed: %s\n", esp_err_to_name(err));
        return 0;
    }
    // Non-printables → '.', so a Septentrio `$R:` reply stays legible even when
    // RTCM3 binary from a streaming port is teed into the capture alongside it.
    printf("--- reply (%u B) ---\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        char c = reply[i];
        putchar((c == '\r' || c == '\n' || (c >= 0x20 && c < 0x7f)) ? c : '.');
    }
    printf("\n--- end ---\n");
    return 0;
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) { printf("board I2C bus not up\n"); return 0; }
    printf("scanning Tab5 system I2C (SDA31/SCL32):\n");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            const char *hint = "";
            switch (a) {
            case 0x43: hint = " (PI4IOE #1 — LCD/TP/CAM reset)"; break;
            case 0x44: hint = " (PI4IOE #2 — USB5V/WLAN)"; break;
            case 0x5D: case 0x14: hint = " (GT911 touch — ILI9881C rev?)"; break;
            case 0x48: hint = " (touch/other)"; break;
            default: break;
            }
            printf("  0x%02X%s\n", a, hint);
            found++;
        }
    }
    printf("%d device(s)\n", found);
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!touch_present()) {
        printf("no touch controller (GT911 rev, or not up)\n");
        return 0;
    }
    bool valid = false; int x = 0, y = 0;
    esp_err_t err = touch_read_point(&valid, &x, &y);
    if (err != ESP_OK) {
        printf("touch read error: %s\n", esp_err_to_name(err));
        return 1;
    }
    int64_t idle_ms = (esp_timer_get_time() - touch_last_activity_us()) / 1000;
    printf("touch: %s", valid ? "DOWN" : "up");
    if (valid) printf("  x=%d y=%d", x, y);
    printf("   last activity %lld ms ago\n", (long long)idle_ms);
    return 0;
}

static int cmd_disp(int argc, char **argv)
{
    // disp [red|green|blue|white|black|<hex RGB565>] [backlight%]
    uint16_t c = 0xFFFF;
    if (argc >= 2) {
        if      (!strcmp(argv[1], "red"))   c = 0xF800;
        else if (!strcmp(argv[1], "green")) c = 0x07E0;
        else if (!strcmp(argv[1], "blue"))  c = 0x001F;
        else if (!strcmp(argv[1], "white")) c = 0xFFFF;
        else if (!strcmp(argv[1], "black")) c = 0x0000;
        else c = (uint16_t)strtol(argv[1], NULL, 16);
    }
    int bl = (argc >= 3) ? atoi(argv[2]) : 100;
    display_fill(c);
    backlight_manual(bl);   // set + suspend the adaptive controller (debug)
    printf("filled 0x%04X, backlight %d%%\n", c, bl);
    return 0;
}

static int cmd_wifiset(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifiset <ssid> [password]\n");
        return 0;
    }
    printf("saving WiFi credentials and rebooting to connect...\n");
    wifi_set_creds(argv[1], argc >= 3 ? argv[2] : "");  // does not return (reboots)
    return 0;
}

static int cmd_wifireset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("erasing WiFi credentials and rebooting...\n");
    wifi_forget();  // does not return (reboots)
    return 0;
}

static int cmd_upstreamset(int argc, char **argv)
{
    if (argc < 5) {
        printf("usage: upstreamset <host> <port> <mount> <password>\n");
        printf("  e.g. upstreamset rtk.toiso.fit 2101 TAB5 <pw>\n");
        return 0;
    }
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) { printf("bad port: %s\n", argv[2]); return 0; }
    upstream_set_creds(argv[1], (uint16_t)port, argv[3], argv[4]);
    printf("upstream creds saved (%s:%d /%s) — connecting on next cycle\n",
           argv[1], port, argv[3]);
    return 0;
}

static int cmd_upstream(int argc, char **argv)
{
    (void)argc; (void)argv;
    upstream_status_t s;
    upstream_status(&s);
    printf("upstream: %s  provisioned=%d connected=%d\n",
           s.last_msg, s.provisioned, s.connected);
    if (s.provisioned) {
        printf("  target=%s:%u /%s  sent=%llu B  reconnects=%lu\n",
               s.host, s.port, s.mount,
               (unsigned long long)s.bytes_sent, (unsigned long)s.reconnects);
    }
    return 0;
}

static int cmd_upstreamreset(int argc, char **argv)
{
    (void)argc; (void)argv;
    upstream_forget();
    printf("upstream credentials erased (task returns to idle)\n");
    return 0;
}

static int cmd_wifidrop(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("forcing WiFi disconnect — watch for reconnect + GOT_IP\n");
    wifi_sta_drop();
    return 0;
}

static int cmd_upstreamdrop(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("forcing cloud-upstream drop — watch for backoff reconnect\n");
    upstream_drop();
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
        { .command = "mosaic", .help = "send a raw Septentrio command to the Mosaic + print reply", .hint = "<command>", .func = cmd_mosaic },
        { .command = "disp", .help = "fill the panel with a color (light-up test): disp <red|green|blue|white|black|hex> [bl%]", .hint = "[color] [bl%]", .func = cmd_disp },
        { .command = "touch", .help = "read the touch point + idle time (idle-off test)", .func = cmd_touch },
        { .command = "i2cscan", .help = "probe the Tab5 system I2C bus (identify touch IC -> panel rev)", .func = cmd_i2cscan },
        { .command = "wifiset", .help = "set WiFi creds + reboot to join: wifiset <ssid> [pass]", .hint = "<ssid> [pass]", .func = cmd_wifiset },
        { .command = "wifireset", .help = "erase stored WiFi creds + reboot", .func = cmd_wifireset },
        { .command = "wifidrop", .help = "force a STA disconnect (reconnect test)", .func = cmd_wifidrop },
        { .command = "upstreamdrop", .help = "force a cloud-upstream drop (backoff reconnect test)", .func = cmd_upstreamdrop },
        { .command = "upstreamset", .help = "push base RTCM3 to a cloud caster: upstreamset <host> <port> <mount> <pass>", .hint = "<host> <port> <mount> <pass>", .func = cmd_upstreamset },
        { .command = "upstream", .help = "cloud-upstream link state", .func = cmd_upstream },
        { .command = "upstreamreset", .help = "erase stored upstream creds", .func = cmd_upstreamreset },
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
    repl_config.max_cmdline_length = 256;   // Septentrio cmd lines get long

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
