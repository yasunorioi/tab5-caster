// mosaic_config.c — see mosaic_config.h.

#include "mosaic_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"

#include "usb_cdc_source.h"

static const char *TAG = "mosaic_cfg";

// The Septentrio connection descriptor the box's USB host reads. On the
// mosaic-go the composite device's streaming CDC interface (itf2 on the bench)
// carries port USB1 — verified live: this is the only port whose configured
// output (MSM7 all-GNSS + 1006/1033/1230) matches the RTCM3 the box latches.
#define RTCM_PORT "USB1"

// The base-station message set. MSM7 obs for every constellation the mosaic-go
// tracks + reference-station coord/descriptor/bias + ephemeris for the four
// constellations the FKP engine uses (GPS/GLONASS/BeiDou/Galileo) plus QZSS
// (regionally relevant). Order is cosmetic — the receiver re-sorts numerically.
#define RTCM_MSGS \
    "RTCM1006+RTCM1033+RTCM1230+" \
    "RTCM1077+RTCM1087+RTCM1097+RTCM1107+RTCM1117+RTCM1127+RTCM1137+" \
    "RTCM1019+RTCM1020+RTCM1042+RTCM1044+RTCM1046"

// NMEA for the on-panel GNSS view (skyplot + C/N0 bars). GSV carries per-
// satellite azimuth/elevation/SNR; GGA carries the base position. Emitted on a
// SEPARATE port (USB2 = itf4) so the RTCM3 caster stream on USB1 stays pure —
// the box opens itf4 read-only for display (see nmea_source.c). USB2 by name, so
// it applies regardless of which command interface we send it on.
#define NMEA_PORT   "USB2"
#define NMEA_STREAM "Stream1"
#define NMEA_MSGS   "GGA+GSV"
#define NMEA_RATE   "sec1"

// Find `needle` in the first `len` bytes of `hay` (which may contain NUL/binary,
// since a reply captured from a streaming port carries teed RTCM3 bytes).
static bool buf_contains(const char *hay, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return false;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    }
    return false;
}

// Best-effort: enable GGA+GSV on USB2 for the panel's GNSS view. Sent on the
// command channel (USB1/itf2); targets USB2 by name. Logs only — a failure here
// costs the display feature, never the RTCM3 caster path.
static void provision_nmea(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "setNMEAOutput, " NMEA_STREAM ", " NMEA_PORT ", " NMEA_MSGS ", " NMEA_RATE);
    char reply[256];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NMEA provision TX failed: %s", esp_err_to_name(err));
        return;
    }
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected the NMEA config ($R?)");
        return;
    }
    ESP_LOGI(TAG, "Mosaic NMEA output provisioned on " NMEA_PORT " (" NMEA_MSGS ")");
}

esp_err_t mosaic_provision(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "setRTCMv3Output, " RTCM_PORT ", " RTCM_MSGS);

    char reply[512];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provision TX failed: %s", esp_err_to_name(err));
        return err;
    }

    // Septentrio acks a good command with "$R:" and rejects with "$R?". Scan the
    // raw capture (not a C-string — it may hold binary) for the error marker.
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected the RTCM3 config ($R?)");
        return ESP_FAIL;
    }
    if (n == 0) {
        // The port accepted the bytes but answered nothing — it isn't the
        // receiver's command interface (the mosaic-go's first CDC COM is silent
        // both ways). Signal the caller to retry on the next interface it opens.
        ESP_LOGW(TAG, "no reply on this interface — not a command port");
        return ESP_ERR_TIMEOUT;
    }
    if (!buf_contains(reply, n, "$R:")) {
        // Bytes but no ack — likely buried under RTCM3 binary on an already-
        // streaming port. The command almost certainly applied; best-effort OK.
        ESP_LOGW(TAG, "no $R: ack in %u B reply (buried in stream?) — assuming applied", (unsigned)n);
        provision_nmea();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Mosaic RTCM3 output provisioned on " RTCM_PORT " (MSM7 + eph)");
    provision_nmea();
    return ESP_OK;
}
