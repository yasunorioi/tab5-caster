// tab5_ili9881c_init.h — the Tab5's panel-specific ILI9881C init table.
//
// The M5Stack Tab5's exact 720x1280 MIPI-DSI panel needs this vendor init
// sequence (gamma/power/gate timing); the esp_lcd_ili9881c driver's built-in
// generic init lights a different panel. Passed to the driver via
// ili9881c_vendor_config_t.init_cmds. Table copied verbatim from
// M5Tab5-UserDemo — see tab5_ili9881c_init.c.

#pragma once

#include <stddef.h>
#include "esp_lcd_ili9881c.h"

extern const ili9881c_lcd_init_cmd_t tab5_ili9881c_init[];
extern const size_t tab5_ili9881c_init_len;
