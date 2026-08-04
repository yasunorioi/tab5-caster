// caster.h — firmware ↔ Zig ntripcaster interface.
//
// The Zig caster (libntripcaster.a, built from ~/ntripcaster) exposes a tiny
// C-ABI surface. The firmware calls caster_start() once the netif is up and
// rtcm_sink is being fed; the Zig side calls back into caster_console_write()
// for its log output (it has no stderr on this target).

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the caster: spawns the TCP listener (rovers pull /MOSAIC) and the
// local-source feeder (drains rtcm_sink_read() into the mount). Non-blocking;
// idempotent. Returns 0 on success, negative on failure. Implemented in Zig
// (src/embedded.zig).
int caster_start(void);

// Log sink the Zig caster writes its lines to. Implemented in caster_glue.c;
// maps to the ESP-IDF console (stdout). Declared here so both sides agree on
// the signature.
void caster_console_write(const char *data, size_t len);

#ifdef __cplusplus
}
#endif
