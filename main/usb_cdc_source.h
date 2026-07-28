// usb_cdc_source.h — USB Host CDC-ACM driver for the Septentrio mosaic-go.
//
// Brings up the ESP32-P4 USB2.0 OTG in Host mode, opens the Mosaic's virtual
// COM (CDC-ACM) interface, and forwards every received byte into rtcm_sink.
// The Mosaic exposes a COMPOSITE device (several CDC-ACM serial ports + a USB
// network interface for its web UI) — we bind ONE CDC interface, the one
// configured to stream RTCM3.

#pragma once

#include "esp_err.h"

// Starts two tasks: the USB host library event pump, and the CDC connect/read
// loop. Returns after the host stack is installed; device attach is async.
// rtcm_sink_init() must have been called first.
esp_err_t usb_cdc_source_start(void);
