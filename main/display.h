// display.h — M5Stack Tab5 5" 720x1280 MIPI-DSI status panel (ILI9881C).
//
// Bring-up order (all P4-specific): acquire the MIPI D-PHY LDO (chan 3 @ 2500mV)
// -> new DSI bus (2 lanes @ 730 Mbps) -> DBI command IO -> ILI9881C panel with
// the Tab5 vendor init table -> backlight (LEDC on GPIO22). Panel reset is on
// the PI4IOE #1 I/O expander (0x43, P4=LCD_RST), driven over the shared board
// I2C bus — so call after board_power_init().
//
// De-gated like WiFi: display_init() returns an error instead of aborting, and
// app_main runs it off the critical path, so a dead/absent panel can never stop
// the USB->caster core (offline autonomy).

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "esp_lcd_types.h"

// Native panel geometry (portrait). The UI rotates to landscape in software.
#define TAB5_LCD_H_RES 720
#define TAB5_LCD_V_RES 1280

// Bring up the panel + backlight. Safe to call once, after board_power_init().
esp_err_t display_init(void);

// Panel + command-IO handles for the LVGL layer. NULL until display_init() ok.
esp_lcd_panel_handle_t    display_panel(void);
esp_lcd_panel_io_handle_t display_io(void);

// Solid RGB565 fill — the first light-up check, before any LVGL is wired.
void display_fill(uint16_t color565);

// Backlight duty, 0..100%.
void display_backlight(int percent);
