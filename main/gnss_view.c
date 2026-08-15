// gnss_view.c — see gnss_view.h.

#include "gnss_view.h"
#include "nmea_source.h"

#include <math.h>
#include <stdio.h>

// Skyplot geometry (portrait 720 wide). Square, centred horizontally. Kept
// smaller than the panel width so the status text + C/N0 strip fit above/below.
#define SKY_SIZE   460
#define SKY_MARGIN 14
#define SKY_R      ((SKY_SIZE / 2) - SKY_MARGIN)   // el=0 radius
#define DOT_D      14                              // satellite dot diameter

// C/N0 bar strip. Absolute-positioned (NOT flex): a per-tick flex relayout of
// ~30 columns tripped the LVGL task watchdog, so bars + PRN labels are placed
// by hand — cheap invalidation, no layout pass.
#define BAR_ROW_W  660
#define BAR_AREA_H 168
#define BAR_LBL_H  20
#define BAR_GAP    2
#define BAR_BASE   (BAR_AREA_H - BAR_LBL_H)   // y of the bar baseline / label top
#define BAR_MAX    52.0f    // dB-Hz mapped to full bar height
#define BAR_MIN    20.0f    // dB-Hz mapped to zero height

#define MAX_DOTS   NMEA_MAX_SATS
#define MAX_BARS   NMEA_MAX_SATS

static lv_obj_t *s_sky;                 // skyplot square (absolute layout)
static lv_obj_t *s_dots[MAX_DOTS];
static lv_obj_t *s_barrow;              // plain container (absolute layout)
static lv_obj_t *s_bar[MAX_BARS];
static lv_obj_t *s_barlbl[MAX_BARS];
static lv_obj_t *s_caption;             // "GNSS  N sats  fixq"

static uint32_t con_color(char t)
{
    switch (t) {
    case 'G': return 0x33DD66;   // GPS      green
    case 'R': return 0xFF5555;   // GLONASS  red
    case 'E': return 0x3399FF;   // Galileo  blue
    case 'C': return 0xFFCC33;   // BeiDou   amber
    case 'J': return 0xCC66FF;   // QZSS     purple
    case 'I': return 0xFF9933;   // NavIC    orange
    default:  return 0x8899AA;   // unknown  grey
    }
}

static void ring(lv_obj_t *parent, int diameter, uint8_t opa)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, diameter, diameter);
    lv_obj_center(c);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x44546A), 0);
    lv_obj_set_style_border_opa(c, opa, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
}

static void cardinal(lv_obj_t *parent, const char *txt, lv_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8899AA), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, align, 0, 0);
}

void gnss_view_build(lv_obj_t *parent)
{
    s_caption = lv_label_create(parent);
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_caption, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_top(s_caption, 8, 0);
    lv_label_set_text(s_caption, "GNSS");

    // Skyplot square.
    s_sky = lv_obj_create(parent);
    lv_obj_remove_style_all(s_sky);
    lv_obj_set_size(s_sky, SKY_SIZE, SKY_SIZE);
    lv_obj_clear_flag(s_sky, LV_OBJ_FLAG_SCROLLABLE);

    ring(s_sky, SKY_R * 2, 255);                       // horizon (el 0)
    ring(s_sky, (int)(SKY_R * 2 * (60.0 / 90)), 160);  // el 30
    ring(s_sky, (int)(SKY_R * 2 * (30.0 / 90)), 160);  // el 60
    cardinal(s_sky, "N", LV_ALIGN_TOP_MID);
    cardinal(s_sky, "S", LV_ALIGN_BOTTOM_MID);
    cardinal(s_sky, "E", LV_ALIGN_RIGHT_MID);
    cardinal(s_sky, "W", LV_ALIGN_LEFT_MID);

    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_t *d = lv_obj_create(s_sky);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, DOT_D, DOT_D);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = d;
    }

    // C/N0 bars: a plain container; each satellite gets a vertical bar with a
    // PRN label beneath it (RTKLIB-style), all absolute-positioned in update.
    s_barrow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_barrow);
    lv_obj_set_size(s_barrow, BAR_ROW_W, BAR_AREA_H);
    lv_obj_set_style_pad_top(s_barrow, 8, 0);
    lv_obj_clear_flag(s_barrow, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < MAX_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(s_barrow);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_size(bar, 6, 4);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(s_barrow);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xC8D2DC), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, "");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        s_bar[i] = bar;
        s_barlbl[i] = lbl;
    }
}

void gnss_view_update(void)
{
    if (!s_sky) return;
    nmea_status_t s;
    nmea_source_status(&s);

    const int cx = SKY_SIZE / 2, cy = SKY_SIZE / 2;

    // Skyplot: place a dot per satellite with a known elevation.
    int nd = 0;
    for (uint8_t i = 0; i < s.sat_count && nd < MAX_DOTS; i++) {
        int el = s.sats[i].elev, az = s.sats[i].azim;
        if (el < 0 || el > 90 || az < 0) continue;
        float r = SKY_R * (90 - el) / 90.0f;
        float a = az * (float)M_PI / 180.0f;
        int x = cx + (int)(r * sinf(a));
        int y = cy - (int)(r * cosf(a));
        lv_obj_t *d = s_dots[nd++];
        lv_obj_set_style_bg_color(d, lv_color_hex(con_color(s.sats[i].talker)), 0);
        lv_obj_set_pos(d, x - DOT_D / 2, y - DOT_D / 2);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = nd; i < MAX_DOTS; i++) lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);

    // C/N0 bars: one per tracked satellite (cn0 known), placed left-to-right.
    int nb = 0;
    for (uint8_t i = 0; i < s.sat_count; i++) if (s.sats[i].cn0 >= 0) nb++;
    int bw = nb > 0 ? (BAR_ROW_W - (nb + 1) * BAR_GAP) / nb : 16;
    if (bw < 6) bw = 6;
    if (bw > 26) bw = 26;
    int usable = BAR_BASE - 6;
    int k = 0;
    for (uint8_t i = 0; i < s.sat_count && k < MAX_BARS; i++) {
        if (s.sats[i].cn0 < 0) continue;
        float f = (s.sats[i].cn0 - BAR_MIN) / (BAR_MAX - BAR_MIN);
        if (f < 0.05f) f = 0.05f;
        if (f > 1.0f) f = 1.0f;
        int h = (int)(f * usable);
        int x = BAR_GAP + k * (bw + BAR_GAP);
        uint32_t col = con_color(s.sats[i].talker);

        lv_obj_set_size(s_bar[k], bw, h);
        lv_obj_set_pos(s_bar[k], x, BAR_BASE - h);
        lv_obj_set_style_bg_color(s_bar[k], lv_color_hex(col), 0);
        lv_obj_clear_flag(s_bar[k], LV_OBJ_FLAG_HIDDEN);

        char t[4];
        snprintf(t, sizeof(t), "%u", s.sats[i].prn);
        lv_label_set_text(s_barlbl[k], t);
        lv_obj_set_style_text_color(s_barlbl[k], lv_color_hex(col), 0);
        lv_obj_set_width(s_barlbl[k], bw + BAR_GAP);
        lv_obj_set_pos(s_barlbl[k], x - BAR_GAP / 2, BAR_BASE + 2);
        lv_obj_clear_flag(s_barlbl[k], LV_OBJ_FLAG_HIDDEN);
        k++;
    }
    for (int i = k; i < MAX_BARS; i++) {
        lv_obj_add_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_barlbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    char cap[64];
    snprintf(cap, sizeof(cap), "GNSS  %u sats  fix %d", s.sat_count, s.fix_quality);
    lv_label_set_text(s_caption, cap);
}
