// status_screen.c — see status_screen.h.

#include "status_screen.h"
#include "display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "rtcm_monitor.h"
#include "usb_cdc_source.h"
#include "caster.h"
#include "upstream.h"
#include "wifi_sta.h"
#include "gnss_view.h"

static const char *TAG = "status_ui";

// Palette. C_BG is chosen to survive RGB565 quantization identically to the
// boot pre-fill (display_fill 0x08A4) so the area below the widgets — which LVGL
// leaves untouched — is a seamless match to the LVGL-painted background.
#define C_BG      0x081420
#define C_TITLE   0xFFFFFF
#define C_OK      0x33DD66
#define C_WARN    0xFFCC33
#define C_BAD     0xFF5555
#define C_IDLE    0x8899AA

static lv_obj_t *s_caster, *s_rtcm, *s_gnss, *s_wifi, *s_cloud, *s_mosaic;

// Rate state (frames/s, cloud KB/s) between refreshes.
static uint64_t s_prev_frames;
static uint64_t s_prev_up_bytes;
static int64_t  s_prev_us;

static lv_obj_t *mk_line(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_IDLE), 0);
    lv_obj_set_style_pad_bottom(l, 14, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, "");
    return l;
}

static void set_line(lv_obj_t *l, uint32_t color, const char *text)
{
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 28, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_pad_bottom(title, 28, 0);
    lv_label_set_text(title, "TAB5 RTK BASE");

    s_caster = mk_line(scr);
    s_rtcm   = mk_line(scr);
    s_gnss   = mk_line(scr);
    s_wifi   = mk_line(scr);
    s_cloud  = mk_line(scr);
    s_mosaic = mk_line(scr);

    // GNSS skyplot + C/N0 bars below the status lines (portrait has the room).
    gnss_view_build(scr);
}

// Build a compact present-constellations string from the monitor's type tally.
static void gnss_string(const rtcm_mon_stats_t *m, char *buf, size_t n)
{
    static const struct { uint16_t type; char c; } msm7[] = {
        {1077, 'G'}, {1087, 'R'}, {1097, 'E'}, {1127, 'C'},
        {1107, 'S'}, {1117, 'J'}, {1137, 'I'},
    };
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t k = 0; k < sizeof(msm7) / sizeof(msm7[0]); k++) {
        bool present = false;
        for (uint32_t i = 0; i < m->type_count; i++) {
            if (m->types[i].type == msm7[k].type) { present = true; break; }
        }
        if (present && pos + 2 < n) {
            if (pos) buf[pos++] = ' ';
            buf[pos++] = msm7[k].c;
            buf[pos] = '\0';
        }
    }
    if (pos == 0) strlcpy(buf, "none", n);
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    char buf[96];
    int64_t now = esp_timer_get_time();
    double dt = (s_prev_us && now > s_prev_us) ? (now - s_prev_us) / 1e6 : 1.0;

    // Caster + rovers.
    caster_source_stats_t cs;
    caster_source_stats(&cs);
    uint32_t col = cs.running ? (cs.source_present ? C_OK : C_WARN) : C_BAD;
    snprintf(buf, sizeof(buf), "Caster  %s   rovers %u",
             cs.running ? (cs.source_present ? "LISTEN" : "no src") : "DOWN",
             cs.client_count);
    set_line(s_caster, col, buf);

    // RTCM rate + CRC health.
    rtcm_mon_stats_t ms;
    rtcm_monitor_get(&ms);
    double rate = (ms.valid_frames >= s_prev_frames) ?
                  (ms.valid_frames - s_prev_frames) / dt : 0.0;
    int64_t age_ms = ms.last_frame_us ? (now - ms.last_frame_us) / 1000 : -1;
    bool fresh = (age_ms >= 0 && age_ms < 3000);
    col = fresh ? (ms.crc_fails == 0 ? C_OK : C_WARN) : C_BAD;
    snprintf(buf, sizeof(buf), "RTCM   %.1f/s   CRCerr %llu",
             rate, (unsigned long long)ms.crc_fails);
    set_line(s_rtcm, col, buf);

    // Constellations.
    char g[32];
    gnss_string(&ms, g, sizeof(g));
    snprintf(buf, sizeof(buf), "GNSS   %s", g);
    set_line(s_gnss, fresh ? C_OK : C_IDLE, buf);

    // WiFi.
    wifi_status_t w;
    wifi_sta_status(&w);
    if (w.connected) {
        snprintf(buf, sizeof(buf), "WiFi   %s  %s", w.ssid, w.ip);
        set_line(s_wifi, C_OK, buf);
    } else if (w.ssid[0]) {
        snprintf(buf, sizeof(buf), "WiFi   %s  connecting...", w.ssid);
        set_line(s_wifi, C_WARN, buf);
    } else {
        set_line(s_wifi, C_IDLE, "WiFi   not set (wifiset)");
    }

    // Cloud upstream.
    upstream_status_t u;
    upstream_status(&u);
    if (u.connected) {
        double kbs = (u.bytes_sent >= s_prev_up_bytes) ?
                     (u.bytes_sent - s_prev_up_bytes) / dt / 1024.0 : 0.0;
        snprintf(buf, sizeof(buf), "Cloud  /%s UP %.1fKB/s  rc%lu",
                 u.mount, kbs, (unsigned long)u.reconnects);
        set_line(s_cloud, C_OK, buf);
    } else if (u.provisioned) {
        snprintf(buf, sizeof(buf), "Cloud  /%s DOWN  rc%lu",
                 u.mount, (unsigned long)u.reconnects);
        set_line(s_cloud, C_WARN, buf);
    } else {
        set_line(s_cloud, C_IDLE, "Cloud  not set (upstreamset)");
    }

    // Mosaic / USB.
    usb_cdc_status_t us;
    usb_cdc_source_status(&us);
    if (us.cdc_open && us.stream_itf != 0xFF) {
        snprintf(buf, sizeof(buf), "Mosaic %04X:%04X  itf%d", us.vid, us.pid,
                 (int8_t)us.stream_itf);
        set_line(s_mosaic, C_OK, buf);
    } else if (us.device_attached) {
        set_line(s_mosaic, C_WARN, "Mosaic attached, searching COM...");
    } else {
        set_line(s_mosaic, C_BAD, "Mosaic —  (no receiver)");
    }

    s_prev_frames  = ms.valid_frames;
    s_prev_up_bytes = u.bytes_sent;
    s_prev_us      = now;

    gnss_view_update();   // skyplot + C/N0 bars from the NMEA snapshot
}

esp_err_t status_screen_start(void)
{
    const lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&pcfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = display_io(),
        .panel_handle  = display_panel(),
        .buffer_size   = TAB5_LCD_H_RES * 120,   // partial buffer, PSRAM
        .double_buffer = true,
        .hres          = TAB5_LCD_H_RES,
        .vres          = TAB5_LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags = { .avoid_tearing = false } };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) { ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed"); return ESP_FAIL; }

    if (!lvgl_port_lock(1000)) { ESP_LOGE(TAG, "lvgl lock timeout"); return ESP_FAIL; }
    build_ui();
    refresh_cb(NULL);                       // paint once immediately
    lv_timer_create(refresh_cb, 1000, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "status UI up");
    return ESP_OK;
}
