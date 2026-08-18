// wifi_sta.h — join the field WiFi via the onboard ESP32-C6 (ESP-Hosted).
//
// Credentials live in NVS, not the firmware. Set them at runtime over the USB
// console (`wifiset <ssid> <pass>`); the box then joins STA every boot and
// starts the caster on GOT_IP. Requires esp_netif_init() + the default event
// loop first (app_main does that). (SoftAP field-provisioning is deferred:
// AP mode over ESP-Hosted on the C6 is broken upstream.)

#pragma once

#include <stdbool.h>

// Snapshot of the WiFi link, for the status UI.
typedef struct {
    bool connected;     // GOT_IP received and still associated
    char ssid[33];      // joined SSID (empty if no creds)
    char ip[16];        // dotted IPv4 ("" until GOT_IP)
} wifi_status_t;

void wifi_sta_status(wifi_status_t *out);

// Bring up WiFi STA from stored (NVS) credentials. If none are stored, the C6
// link still comes up and the box idles until provisioned via wifi_set_creds().
void wifi_sta_start(void);

// Force a STA disconnect to exercise the auto-reconnect path (the `wifidrop`
// console command). The event handler re-associates immediately — a reconnect
// test hook, usable on the bench and in the field. No effect if WiFi isn't up.
void wifi_sta_drop(void);

// Store credentials and reboot to join (the `wifiset` console command).
void wifi_set_creds(const char *ssid, const char *pass);

// Forget stored credentials and reboot (the `wifireset` console command).
void wifi_forget(void);

// Save / erase creds WITHOUT rebooting — for callers that must send a response
// first (e.g. the /admin web handler) and trigger the reboot themselves.
void wifi_save_creds(const char *ssid, const char *pass);
void wifi_clear_creds(void);
