// status_screen.h — LVGL status UI on the Tab5 panel.
//
// Brings up LVGL (esp_lvgl_port) bound to the MIPI-DSI panel from display.c,
// builds a one-glance status page, and refreshes it ~1 Hz from the box's live
// state: caster/rover count, RTCM rate + CRC health + constellations, WiFi/IP,
// and the cloud upstream link. Call after a successful display_init().

#pragma once

#include "esp_err.h"

// Init LVGL, attach the DSI panel, build + start the status page. Returns an
// error (does not abort) if LVGL or the display binding fails.
esp_err_t status_screen_start(void);
