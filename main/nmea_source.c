// nmea_source.c — see nmea_source.h.

#include "nmea_source.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "nmea";

// Same composite device as the RTCM3 source; itf4 is the NMEA (USB2) COM.
#define MOSAIC_VID  0x152A
#define MOSAIC_PID  0x85C0
#define NMEA_ITF    4
#define NMEA_BAUD   460800

static SemaphoreHandle_t s_lock;
static nmea_status_t     s_st;             // guarded by s_lock
static SemaphoreHandle_t s_disconnected;
static char              s_line[128];      // NMEA line assembly (cb context)
static size_t            s_linepos;

// ── talker → constellation letter ────────────────────────────────────────────
static char talker_of(const char *f0)   // f0 like "$GPGSV"
{
    if (f0[0] != '$') return '?';
    char a = f0[1], b = f0[2];
    if (a == 'G' && b == 'P') return 'G';
    if (a == 'G' && b == 'L') return 'R';
    if (a == 'G' && b == 'A') return 'E';
    if (a == 'G' && b == 'B') return 'C';
    if (a == 'B' && b == 'D') return 'C';
    if (a == 'G' && b == 'Q') return 'J';
    if (a == 'G' && b == 'I') return 'I';
    if (a == 'G' && b == 'N') return 'N';
    return '?';
}

// Upsert a satellite by (talker,prn). Caller holds s_lock.
static void sat_upsert(char talker, uint8_t prn, int16_t el, int16_t az, int16_t cn)
{
    for (uint8_t i = 0; i < s_st.sat_count; i++) {
        if (s_st.sats[i].talker == talker && s_st.sats[i].prn == prn) {
            s_st.sats[i].elev = el; s_st.sats[i].azim = az; s_st.sats[i].cn0 = cn;
            return;
        }
    }
    if (s_st.sat_count < NMEA_MAX_SATS) {
        s_st.sats[s_st.sat_count++] = (nmea_sat_t){talker, prn, el, az, cn};
    }
}

// Split `line` in place into up to `max` comma fields (stopping at '*'). Returns
// the field count. Empty fields yield "".
static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '*') { *p = '\0'; break; }
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
    }
    return n;
}

// GSV: $xxGSV,total,num,inview,{prn,el,az,cn}×(≤4)
static void parse_gsv(char **f, int nf)
{
    char talker = talker_of(f[0]);
    for (int i = 4; i + 3 < nf; i += 4) {
        if (f[i][0] == '\0') continue;              // empty slot
        uint8_t prn = (uint8_t)atoi(f[i]);
        int16_t el  = f[i+1][0] ? (int16_t)atoi(f[i+1]) : -1;
        int16_t az  = f[i+2][0] ? (int16_t)atoi(f[i+2]) : -1;
        int16_t cn  = f[i+3][0] ? (int16_t)atoi(f[i+3]) : -1;
        if (prn) sat_upsert(talker, prn, el, az, cn);
    }
}

// GGA: $xxGGA,time,lat,N/S,lon,E/W,fixq,nsats,...
static double dm_to_deg(const char *dm, const char *hemi)
{
    if (!dm[0]) return 0.0;
    double v = atof(dm);
    int deg = (int)(v / 100);
    double min = v - deg * 100;
    double d = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
    return d;
}

static void parse_gga(char **f, int nf)
{
    if (nf < 8) return;
    s_st.lat = dm_to_deg(f[2], f[3]);
    s_st.lon = dm_to_deg(f[4], f[5]);
    s_st.fix_quality = atoi(f[6]);
    s_st.gga_sats = atoi(f[7]);
}

static void process_line(char *line)
{
    // A valid sentence is at least "$xxYYY".
    if (line[0] != '$' || strlen(line) < 6) return;
    char *fields[40];
    // Keep a copy for split (split mutates); also stash the raw as last_line.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.last_line, line, sizeof(s_st.last_line));
    int nf = split_fields(line, fields, 40);
    const char *code = fields[0] + 3;               // after "$xx"
    if (strncmp(code, "GSV", 3) == 0) {
        s_st.gsv_sentences++;
        parse_gsv(fields, nf);
    } else if (strncmp(code, "GGA", 3) == 0) {
        s_st.gga_sentences++;
        parse_gga(fields, nf);
    }
    xSemaphoreGive(s_lock);
}

static bool nmea_data_cb(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.bytes += len;
    xSemaphoreGive(s_lock);
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r' || c == '\n') {
            if (s_linepos > 0) {
                s_line[s_linepos] = '\0';
                process_line(s_line);
                s_linepos = 0;
            }
        } else if (s_linepos < sizeof(s_line) - 1) {
            s_line[s_linepos++] = (char)c;
        } else {
            s_linepos = 0;   // overlong line — resync
        }
    }
    return true;
}

static void nmea_event_cb(const cdc_acm_host_dev_event_data_t *event, void *ctx)
{
    (void)ctx;
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "itf4 disconnected");
        cdc_acm_host_close(event->data.cdc_hdl);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.itf_open = false;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_disconnected);
        break;
    default:
        break;
    }
}

static void nmea_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 0,
        .in_buffer_size = 2048,
        .event_cb = nmea_event_cb,
        .data_cb = nmea_data_cb,
        .user_arg = NULL,
    };
    const cdc_acm_line_coding_t lc = {
        .dwDTERate = NMEA_BAUD, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
    };

    for (;;) {
        cdc_acm_dev_hdl_t cdc = NULL;
        // Retry until the device is attached AND itf4 is free (the RTCM3 sweep
        // latches itf2 before ever reaching itf4, so this normally succeeds).
        if (cdc_acm_host_open(MOSAIC_VID, MOSAIC_PID, NMEA_ITF, &cfg, &cdc) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "itf4 (NMEA/USB2) open — reading GGA+GSV");
        cdc_acm_host_line_coding_set(cdc, &lc);   // best-effort
        cdc_acm_host_set_control_line_state(cdc, true, true);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.itf_open = true;
        xSemaphoreGive(s_lock);

        xSemaphoreTake(s_disconnected, portMAX_DELAY);   // block until unplug
        vTaskDelay(pdMS_TO_TICKS(1000));                 // let the sweep settle
    }
}

void nmea_source_status(nmea_status_t *out)
{
    if (!out) return;
    if (!s_lock) { *out = (nmea_status_t){0}; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

void nmea_source_start(void)
{
    if (s_lock) return;   // already started
    s_lock = xSemaphoreCreateMutex();
    s_disconnected = xSemaphoreCreateBinary();
    if (!s_lock || !s_disconnected) { ESP_LOGE(TAG, "sync alloc failed"); return; }
    xTaskCreate(nmea_task, "nmea", 4096, NULL, 4, NULL);
}
