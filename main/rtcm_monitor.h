// rtcm_monitor.h — bring-up telemetry for the RTCM3 byte stream.
//
// Fed the same bytes that go to the caster, it validates RTCM3 frames
// (CRC-24Q) and tallies message types. This is the "is the Mosaic actually
// sending the messages the caster needs?" check, on-device, without the caster.
//
// Answers questions like: are 1005/1077/1087/1097/1019 present? Are frames CRC-
// valid or is it noise? How long since the last good frame?

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RTCM_MON_MAX_TYPES   48   // distinct message types tracked
#define RTCM_MON_SNAP_BYTES  256  // last-frame snapshot for `dump`
#define RTCM_MON_RAW_BYTES   128  // rolling raw-byte tap for `raw`

typedef struct {
    uint16_t type;
    uint32_t count;
} rtcm_type_count_t;

typedef struct {
    uint64_t total_bytes;      // all bytes seen
    uint64_t valid_frames;     // CRC-24Q OK
    uint64_t crc_fails;        // 0xD3-led but CRC mismatch
    int64_t  last_frame_us;    // esp_timer time of last valid frame (0 = never)
    uint32_t type_count;       // entries used in `types`
    rtcm_type_count_t types[RTCM_MON_MAX_TYPES];
} rtcm_mon_stats_t;

// Must be called once before feeding.
void rtcm_monitor_init(void);

// Feed raw bytes (call from the sink consumer task). Reassembles across chunks.
void rtcm_monitor_feed(const uint8_t *data, size_t len);

// Copy a consistent snapshot of the counters (thread-safe).
void rtcm_monitor_get(rtcm_mon_stats_t *out);

// Copy the last CRC-valid frame into buf (up to max). Returns its length (0 if
// none yet). Used by the `dump` console command.
size_t rtcm_monitor_last_frame(uint8_t *buf, size_t max);

// Copy the most-recent raw bytes seen (up to max), regardless of RTCM3 validity.
// Returns the count. Lets `raw` inspect a non-RTCM3 stream (NMEA/SBF/echo).
size_t rtcm_monitor_last_raw(uint8_t *buf, size_t max);
