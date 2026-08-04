// rtcm_sink.h — the one-way seam between the USB byte source and the caster.
//
// The USB CDC-ACM driver's ONLY job is to shove received bytes in here. Whoever
// consumes them (milestone-1: a hex-dump task; later: the Zig ntripcaster local
// source feeder) drains the StreamBuffer on the other side. This keeps the risky
// USB code decoupled from the caster — the USB driver can be debugged in
// isolation, and if it dies the caster is unaffected.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

// Create the underlying StreamBuffer. Call once before starting producers or
// consumers. Returns ESP_OK / ESP_ERR_NO_MEM.
esp_err_t rtcm_sink_init(void);

// Producer side (called from the CDC data callback). Non-blocking: pushes as
// many bytes as fit; returns the number actually queued. A short read here means
// the consumer is not keeping up (should not happen at RTCM rates).
size_t rtcm_sink_push(const uint8_t *data, size_t len);

// Consumer side. Blocks up to `timeout` for at least 1 byte, then returns up to
// `max_len` bytes. Returns bytes read (0 on timeout). The one drain task
// (rtcm_feed_task) is the sole reader — a FreeRTOS StreamBuffer allows only one.
size_t rtcm_sink_read(uint8_t *out, size_t max_len, TickType_t timeout);

// ── caster tee (second stage) ────────────────────────────────────────────────
// A StreamBuffer allows a single reader, so the monitor drain task and the Zig
// caster cannot both read the primary sink. Instead the drain task, after
// feeding the monitor, pushes the same bytes here, and the caster reads from
// here. Push is non-blocking (drops while the caster isn't draining yet); read
// is the blocking call the Zig local-source feeder makes (rtcm_caster_read).
size_t rtcm_caster_push(const uint8_t *data, size_t len);
size_t rtcm_caster_read(uint8_t *out, size_t max_len, TickType_t timeout);
