// tab5_st7123_init.h — the Tab5's panel-specific ST7123 init table.
//
// The newer (post-2025-10) M5Stack Tab5 revision uses an ST7123 integrated
// display+touch controller instead of the ILI9881C. This is the PRODUCTION init
// sequence M5 ships (M5Tab5-UserDemo m5stack_tab5.c, st7123_vendor_specific_
// init_default) — SLPOUT/DISPON/TEON are inside it, so the driver must not also
// send them. Passed to the driver via st7123_vendor_config_t.init_cmds.

#pragma once

#include <stddef.h>
#include "esp_lcd_st7123.h"

extern const st7123_lcd_init_cmd_t tab5_st7123_init[];
extern const size_t tab5_st7123_init_len;
