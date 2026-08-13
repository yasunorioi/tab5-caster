// upstream.h — NTRIP source-client: push the base's RTCM3 to a cloud caster.
//
// The box is behind Starlink (no reachable inbound), so the cloud FKP engine
// cannot pull from it. Instead the box opens an OUTBOUND connection to the cloud
// caster and streams its base RTCM3 as an NTRIP v1 SOURCE. This runs alongside —
// and independent of — the local caster: the box keeps serving rovers offline,
// and additionally feeds the cloud for network-RTK/FKP synthesis when online.
//
// Credentials (host/port/mount/password) live in NVS, set at runtime with the
// `upstreamset` console command — never hardcoded. The task idles until
// provisioned, then connects and reconnects with exponential backoff.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start the upstream task (idempotent). Call once the netif is up (GOT_IP).
void upstream_start(void);

// Persist upstream credentials to NVS. Takes effect on the task's next connect
// attempt (no reboot needed). `mount` may be given with or without a leading '/'.
void upstream_set_creds(const char *host, uint16_t port,
                        const char *mount, const char *pass);

// Erase stored upstream credentials (the task returns to idle on next cycle).
void upstream_forget(void);

typedef struct {
    bool     provisioned;   // creds present in NVS
    bool     connected;     // SOURCE handshake done, streaming
    char     host[64];
    uint16_t port;
    char     mount[32];     // without leading '/'
    uint64_t bytes_sent;
    uint32_t reconnects;
    char     last_msg[80];  // last state/error line for the `upstream` command
} upstream_status_t;

void upstream_status(upstream_status_t *out);
