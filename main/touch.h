// touch.h — minimal touch-activity detector for the Tab5 panel.
//
// We don't need touch *coordinates* — only "did the user touch the screen
// recently", to drive the idle backlight-off (see backlight.c). So this polls
// the touch controller for a finger-present flag and timestamps it; no LVGL
// input device, no gesture decoding.
//
// This unit's panel rev is ST7123 (integrated display+touch @ 0x55). The older
// ILI9881C rev pairs with a GT911 (@0x14/0x5D) — not implemented yet; on that
// rev touch_init() reports "absent" and idle-off simply stays disabled.

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Probe the touch controller on the board I2C bus and, if present, start the
// poll task. Call after display_init() (needs the bus up + TP_RST released).
esp_err_t touch_init(void);

// True if a touch controller was found and is being polled.
bool touch_present(void);

// esp_timer timestamp (us) of the last detected finger-down. Seeded to boot
// time at init so "never touched since boot" ages normally. 0 if no touch hw.
int64_t touch_last_activity_us(void);

// One-shot read for the `touch` console command: fills *valid/*x/*y from point 0.
esp_err_t touch_read_point(bool *valid, int *x, int *y);
