// mosaic_config.h — self-provisioning of the Septentrio mosaic-go base output.
//
// The product goal is flash-and-go: plug any mosaic-go into the box and it just
// works, without the operator having to configure the receiver. On boot the box
// pushes its canonical RTCM3 base-station output config to the Mosaic over the
// open CDC command channel, so even a factory-config receiver starts streaming
// what the caster/FKP need.

#pragma once

#include "esp_err.h"

// Configure the Mosaic's RTCM3 output on the USB port the box reads (USB1):
// MSM7 for every constellation + 1006 (station coord) + 1033 (rcv/ant) + 1230
// (GLONASS bias) + ephemeris 1019/1020/1042/1044/1046 (GPS/GLO/BDS/QZSS/GAL).
//
// Applied to the receiver's CURRENT (RAM) config only — NOT saved to its boot
// config. The box re-provisions on every power-up, so it stays the source of
// truth and the Mosaic's own saved state is left untouched (no flash wear).
//
// Requires a CDC interface already open (usb_cdc_send_command). Returns ESP_OK
// if the receiver acknowledged the command ($R:, not $R?).
esp_err_t mosaic_provision(void);
