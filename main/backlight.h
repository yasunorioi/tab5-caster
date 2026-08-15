// backlight.h — power-aware adaptive panel backlight.
//
// The Tab5 panel backlight (a boosted LED string on a P4 GPIO) shares the 5V
// budget that also feeds the Mosaic over USB-A. At full brightness a thin supply
// (e.g. a laptop USB-C port during bench work) browns out on the receiver's
// streaming inrush, dropping it off the USB bus (CDC error) so RTCM3 never
// latches. See the M3-B brownout finding.
//
// This controller keeps the backlight at a safe floor whenever RTCM3 is NOT
// streaming (so the receiver always has headroom to enumerate + start output),
// then slowly climbs toward a target while the stream stays healthy. If the
// stream drops, it treats the current brightness as too high for this supply,
// caps a session ceiling below it, and falls back to the floor. The result
// auto-calibrates to whatever power is present: bright on an ample field supply,
// dim-but-streaming on a thin one — no hard-coded cap, no field UX regression.
//
// Independent of LVGL/status_screen (both de-gated): protecting the RTCM3 core
// must not depend on the UI being up. Only start it once the panel is alive.

#pragma once

// Apply the safe floor immediately and arm the 1 Hz adaptive controller.
// Call after display_init() succeeds.
void backlight_auto_start(void);

// Manual override for the `disp` console command: set brightness now and stop
// the auto controller for the rest of the session (a debug escape hatch).
void backlight_manual(int percent);
