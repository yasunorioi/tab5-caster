// wifi_sta.h — join the field WiFi via the onboard ESP32-C6 (ESP-Hosted).
//
// Credentials live in NVS, not the firmware. Set them at runtime over the USB
// console (`wifiset <ssid> <pass>`); the box then joins STA every boot and
// starts the caster on GOT_IP. Requires esp_netif_init() + the default event
// loop first (app_main does that). (SoftAP field-provisioning is deferred:
// AP mode over ESP-Hosted on the C6 is broken upstream.)

#pragma once

// Bring up WiFi STA from stored (NVS) credentials. If none are stored, the C6
// link still comes up and the box idles until provisioned via wifi_set_creds().
void wifi_sta_start(void);

// Store credentials and reboot to join (the `wifiset` console command).
void wifi_set_creds(const char *ssid, const char *pass);

// Forget stored credentials and reboot (the `wifireset` console command).
void wifi_forget(void);
