// usb_cdc_source.h — USB Host CDC-ACM driver for the Septentrio mosaic-go.
//
// Brings up the ESP32-P4 USB2.0 OTG in Host mode, opens the Mosaic's virtual
// COM (CDC-ACM) interface, and forwards every received byte into rtcm_sink.
// The Mosaic exposes a COMPOSITE device (several CDC-ACM serial ports + a USB
// network interface for its web UI) — we bind ONE CDC interface, the one
// configured to stream RTCM3.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Starts two tasks: the USB host library event pump, and the CDC connect/read
// loop. Returns after the host stack is installed; device attach is async.
// rtcm_sink_init() must have been called first.
esp_err_t usb_cdc_source_start(void);

// Compact per-interface topology summary, filled at attach by walking the
// active config descriptor. Rendered into the periodic liveness line so the
// full interface tree survives the USB-Serial-JTAG console dropping one-shots.
#define USB_TOPO_MAX 112

// Snapshot of USB state for the `usb` console command.
typedef struct {
    bool     host_installed;   // usb_host_install() done
    bool     device_attached;  // some CDC device enumerated
    bool     cdc_open;         // the streaming interface is open
    uint16_t vid;              // last attached device (0 if none)
    uint16_t pid;
    uint8_t  num_interfaces;   // alt-setting-0 interfaces in the active config
    char     topo[USB_TOPO_MAX]; // "N:class/sub" per itf, e.g. "0:02/02 1:0a/00"
    uint8_t  cur_itf;          // currently-open comm interface (0xFF = none)
    uint8_t  stream_itf;       // interface that actually streamed (0xFF = none yet)
} usb_cdc_status_t;

void usb_cdc_source_status(usb_cdc_status_t *out);

// Send an ASCII command to the Mosaic over the currently-open CDC interface and
// collect the receiver's reply. A CR/LF is appended. Bytes arriving while the
// command is in flight are captured into `reply` (NUL-terminated, truncated to
// reply_max-1); the capture ends after ~300 ms of silence or when `reply` fills.
// Used by the `mosaic` console passthrough and by mosaic_provision().
//   ESP_ERR_INVALID_STATE — no CDC interface open yet.
// The Septentrio command interface is line-oriented and accepts commands on the
// same port that streams data, so this works whether or not RTCM3 is flowing.
// `reply_len` (may be NULL) receives the number of bytes captured — use it, not
// strlen(reply), since a port that also streams RTCM3 can tee binary bytes (incl.
// NUL) into the reply alongside the ASCII response.
esp_err_t usb_cdc_send_command(const char *cmd, char *reply, size_t reply_max,
                               size_t *reply_len, uint32_t timeout_ms);
