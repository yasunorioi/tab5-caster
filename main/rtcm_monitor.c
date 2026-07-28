// rtcm_monitor.c — see rtcm_monitor.h.

#include "rtcm_monitor.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

// RTCM3 framing: 0xD3 | 6 reserved bits + 10-bit length | payload | CRC-24Q(3B).
#define RTCM3_PREAMBLE   0xD3
#define RTCM3_HDR_LEN    3
#define RTCM3_CRC_LEN    3
#define RTCM3_MAX_PAYLOAD 1023

// Reassembly buffer: a full frame (max ~1029B) may straddle feed() chunks.
#define PARSE_BUF_LEN    2048

static SemaphoreHandle_t s_lock;
static rtcm_mon_stats_t s_stats;

static uint8_t  s_parse[PARSE_BUF_LEN];
static size_t   s_parse_len;

static uint8_t  s_last_frame[RTCM_MON_SNAP_BYTES];
static size_t   s_last_frame_len;

// CRC-24Q (Qualcomm), poly 0x1864CFB — the RTCM3 frame checksum.
static uint32_t crc24q(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 16;
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    return crc & 0xFFFFFF;
}

static void tally_type(uint16_t type)
{
    for (uint32_t i = 0; i < s_stats.type_count; i++) {
        if (s_stats.types[i].type == type) {
            s_stats.types[i].count++;
            return;
        }
    }
    if (s_stats.type_count < RTCM_MON_MAX_TYPES) {
        s_stats.types[s_stats.type_count].type = type;
        s_stats.types[s_stats.type_count].count = 1;
        s_stats.type_count++;
    }
}

void rtcm_monitor_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_stats, 0, sizeof(s_stats));
    s_parse_len = 0;
    s_last_frame_len = 0;
}

void rtcm_monitor_feed(const uint8_t *data, size_t len)
{
    if (s_lock == NULL || data == NULL || len == 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_stats.total_bytes += len;

    // Append to the reassembly buffer; on overflow keep the tail half so a
    // partial frame at the end survives.
    if (s_parse_len + len > PARSE_BUF_LEN) {
        size_t carry = PARSE_BUF_LEN / 2;
        if (s_parse_len > carry) {
            memmove(s_parse, s_parse + (s_parse_len - carry), carry);
            s_parse_len = carry;
        }
        if (len > PARSE_BUF_LEN) {          // single chunk bigger than buffer
            data += (len - PARSE_BUF_LEN);
            len = PARSE_BUF_LEN;
            s_parse_len = 0;
        }
    }
    memcpy(s_parse + s_parse_len, data, len);
    s_parse_len += len;

    // Scan for complete frames.
    size_t pos = 0;
    while (pos < s_parse_len) {
        if (s_parse[pos] != RTCM3_PREAMBLE) { pos++; continue; }
        if (s_parse_len - pos < RTCM3_HDR_LEN) break;   // header not in yet

        size_t payload_len = ((size_t)(s_parse[pos + 1] & 0x03) << 8) | s_parse[pos + 2];
        if (payload_len > RTCM3_MAX_PAYLOAD) { pos++; continue; }
        size_t frame_len = RTCM3_HDR_LEN + payload_len + RTCM3_CRC_LEN;
        if (s_parse_len - pos < frame_len) break;       // full frame not in yet

        const uint8_t *f = &s_parse[pos];
        uint32_t want = ((uint32_t)f[frame_len - 3] << 16) |
                        ((uint32_t)f[frame_len - 2] << 8)  |
                         (uint32_t)f[frame_len - 1];
        if (crc24q(f, RTCM3_HDR_LEN + payload_len) == want) {
            uint16_t type = ((uint16_t)f[RTCM3_HDR_LEN] << 4) | (f[RTCM3_HDR_LEN + 1] >> 4);
            s_stats.valid_frames++;
            s_stats.last_frame_us = esp_timer_get_time();
            tally_type(type);
            size_t snap = frame_len < RTCM_MON_SNAP_BYTES ? frame_len : RTCM_MON_SNAP_BYTES;
            memcpy(s_last_frame, f, snap);
            s_last_frame_len = snap;
            pos += frame_len;
        } else {
            s_stats.crc_fails++;
            pos++;   // resync one byte at a time
        }
    }

    // Shift out consumed bytes.
    if (pos > 0 && pos < s_parse_len) {
        memmove(s_parse, s_parse + pos, s_parse_len - pos);
        s_parse_len -= pos;
    } else if (pos >= s_parse_len) {
        s_parse_len = 0;
    }

    xSemaphoreGive(s_lock);
}

void rtcm_monitor_get(rtcm_mon_stats_t *out)
{
    if (s_lock == NULL || out == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_stats, sizeof(*out));
    xSemaphoreGive(s_lock);
}

size_t rtcm_monitor_last_frame(uint8_t *buf, size_t max)
{
    if (s_lock == NULL || buf == NULL || max == 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_last_frame_len < max ? s_last_frame_len : max;
    memcpy(buf, s_last_frame, n);
    xSemaphoreGive(s_lock);
    return n;
}
