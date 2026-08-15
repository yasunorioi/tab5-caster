// nmea_source.h — read-only NMEA reader on the Mosaic's second USB COM (itf4 =
// USB2), for the on-panel GNSS view (skyplot + C/N0 bars).
//
// The RTCM3 caster is fed from USB1 (itf2). This opens a SECOND CDC interface,
// itf4, purely to parse GGA (base position) + GSV (per-satellite az/el/SNR) —
// it never touches rtcm_sink, so the caster stream stays pure. GSV is enabled on
// USB2 by mosaic_config.c's provisioning.
//
// Spike scope: prove GSV arrives with az/el/SNR and expose a snapshot for the
// `nmea` console command. The skyplot/bar rendering builds on this snapshot.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char    talker;    // 'G' GPS, 'R' GLONASS, 'E' Galileo, 'C' BeiDou,
                       // 'J' QZSS, 'I' NavIC, 'N' mixed, '?' unknown
    uint8_t prn;
    int16_t elev;      // degrees, -1 = unknown
    int16_t azim;      // degrees
    int16_t cn0;       // dB-Hz, -1 = unknown/not tracked
} nmea_sat_t;

#define NMEA_MAX_SATS 64   // all constellations in view can exceed 40

typedef struct {
    bool     itf_open;         // itf4 currently open
    uint32_t bytes;            // total bytes read on itf4
    uint32_t gsv_sentences;    // GSV sentences parsed
    uint32_t gga_sentences;    // GGA sentences parsed
    uint8_t  sat_count;        // entries in sats[]
    nmea_sat_t sats[NMEA_MAX_SATS];
    char     last_line[96];    // last complete NMEA line (sample/debug)
    // last GGA fix
    double   lat, lon;         // degrees, signed
    int      fix_quality;      // GGA field 6 (0=none,1=GPS,4=RTK fix,5=float,…)
    int      gga_sats;         // satellites used in the fix
} nmea_status_t;

// Start the itf4 reader task (idempotent). Call after usb_cdc_source_start()
// (needs cdc_acm_host installed).
void nmea_source_start(void);

// Snapshot the parsed NMEA state for the `nmea` console command / GNSS view.
void nmea_source_status(nmea_status_t *out);
