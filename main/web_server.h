// web_server.h — read-only HTTP status server (browser view of the box).
//
// A browser-reachable counterpart to the LVGL status panel and the USB console:
// serves GET / (a self-contained dashboard) and GET /api/status (JSON) built
// from the box's existing live accessors (wifi/usb/rtcm/caster/upstream). It is
// strictly read-only — no config writes — so it can never touch the caster path.
//
// Runs on esp_http_server (its own task). De-gated exactly like the panel and
// WiFi: a failure to start is logged, never fatal — the offline caster core must
// keep running even if the web UI can't come up.

#pragma once

#include "esp_err.h"

// TCP port for the status web UI. Advertised over mDNS (_http._tcp) by
// net_mdns_start() and bound by web_server_start(); kept here so both agree on
// one number.
#define WEB_ADMIN_PORT 8080

// Start the HTTP status server (idempotent). Call once a netif has an IP
// (GOT_IP). Returns ESP_OK, or an error if the httpd could not start — the
// caller logs and presses on (the caster does not depend on this).
esp_err_t web_server_start(void);
