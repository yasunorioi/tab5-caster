// gnss_view.h — on-panel GNSS visualization: skyplot + C/N0 bars.
//
// Renders the nmea_source snapshot (per-satellite az/el/C/N0, all
// constellations) into an LVGL skyplot (polar az/el) plus a C/N0 bar strip,
// coloured by constellation. Built under the status screen and refreshed from
// its 1 Hz timer. Pure display — reads nmea_source_status(), touches nothing else.

#pragma once

#include "lvgl.h"

// Build the skyplot + bar widgets under `parent` (called once from build_ui).
void gnss_view_build(lv_obj_t *parent);

// Refresh dot positions/colours + bar heights from the latest NMEA snapshot
// (called each tick from the status screen's refresh_cb).
void gnss_view_update(void);
