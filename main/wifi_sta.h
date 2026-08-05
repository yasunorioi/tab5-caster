// wifi_sta.h — join the field WiFi via the onboard ESP32-C6 (ESP-Hosted).
//
// Credentials live in NVS, not the firmware. On boot: if creds are stored, join
// STA; otherwise come up as a SoftAP ("rtk-setup") serving a small browser
// config page (pick network + password) that saves to NVS and reboots. On
// GOT_IP the caster is started (turnkey). Requires esp_netif_init() + the
// default event loop to be up first (app_main does that).

#pragma once

// Bring up WiFi: STA from stored creds, or the SoftAP setup portal if none.
void wifi_sta_start(void);

// Forget stored credentials and reboot into the setup portal (console command).
void wifi_forget(void);
