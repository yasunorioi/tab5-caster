// net_mdns.h — advertise the caster over mDNS for zero-config field discovery.
//
// Publishes <hostname>.local plus the NTRIP service (_ntrip._tcp) and the admin
// status UI (_http._tcp), so a rover / laptop on the field WiFi can reach the
// caster by name instead of chasing a DHCP-assigned IP.
//
// REQUIRES an active netif (WiFi via C6/ESP-Hosted, or Ethernet). Until network
// bring-up exists (TODO(hw)) mDNS starts but nothing is reachable.

#pragma once

#include <stdint.h>
#include "esp_err.h"

// Start the mDNS responder and register services. Call once, after network is
// up. `hostname` = the .local label (e.g. "rtk" -> rtk.local).
esp_err_t net_mdns_start(const char *hostname, uint16_t caster_port, uint16_t admin_port);
