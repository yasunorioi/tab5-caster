# tab5-caster

NTRIP caster firmware for **M5Stack Tab5 (ESP32-P4)**, fed by a **Septentrio
mosaic-go (mosaic-G5)** base receiver over **USB (CDC-ACM)**.

Sellable, flash-and-go RTK base box: Mosaic + Tab5 in one USB-C cable, no jumper
wiring. Runs a local NTRIP caster so rovers (auto-steer tractors, RTK drones)
keep getting corrections **even when the internet is down** — the offline-autonomy
reason the box exists. FKP / network-RTK synthesis is done in the **cloud**, not
on this box.

> **Status: compiles clean, hardware-untested.** Builds for `esp32p4` on
> ESP-IDF **5.4.4** with `espressif/usb_host_cdc_acm` **2.4.0** (zero warnings).
> Compilation is not runtime proof — everything marked `TODO(hw)` still needs the
> real Tab5 + Mosaic on the bench (VID/PID/interface, HS-OTG port + VBUS, PSRAM).

## Why USB and not UART

UART (3.3V TTL, 3 wires) is the technically shortest path and needs almost no
firmware. USB was chosen anyway because a single USB-C cable is the right
**product** experience for a flash-and-go kit. The cost is this driver: the P4
must act as a USB **Host** and speak CDC-ACM to the Mosaic's composite device.

## Architecture

```
 Mosaic-go (USB composite: CDC-ACM COMs + web-UI network itf)
        │  USB2.0 HS OTG (P4 = Host)
        ▼
 usb_cdc_source.c  ── cdc_acm_host data_cb ──►  rtcm_sink (StreamBuffer)
                                                     │
                                                     ▼
                          milestone-1: rtcm_dump_task (0xD3 counter)
                          later:       Zig ntripcaster local Source feeder
                                       → rovers pull RTCM over WiFi/Ethernet
```

The **StreamBuffer seam** (`rtcm_sink`) is deliberate: the USB byte source and
the caster are decoupled by one buffer. The USB driver's only contract is "push
RTCM3 bytes"; it can be brought up and debugged with zero caster code. Whether
the source is USB CDC or (some day) UART, the downstream is identical.

## The three things that will bite (read before hacking)

1. **Mosaic USB is a composite device** — several CDC-ACM virtual COMs *plus* a
   USB network interface (its web UI). Bind the ONE CDC interface that streams
   RTCM3, and **enable RTCM3 output on that COM in the Mosaic config**, or you
   get "connected but silent". Fill `MOSAIC_VID/PID/CDC_ITF` from the M0 log.
2. **P4 USB host needs IDF 5.4+** and the right Tab5 connector wired to the HS
   OTG PHY. Confirm VBUS: does the box bus-power the Mosaic or supply it
   externally? (`TODO(hw)` in `sdkconfig.defaults` / `usb_cdc_source.c`.)
3. **CDC-ACM host is callback-driven, not a POSIX fd** — hence the StreamBuffer
   instead of wrapping it as an `io.Stream` directly on the USB side.

## Debugging

On-device REPL over the console (UART / USB-Serial-JTAG, same port as
`idf.py monitor`). Type `help`; commands:

| cmd | what |
|-----|------|
| `stats` | byte count, CRC-valid frame count, CRC fails, ms since last good frame |
| `rtcm`  | message-type histogram — instantly shows if `1005/1077/1087/1097/1019` are present (the types the caster/FKP need), annotated |
| `dump [n]` | hexdump the last CRC-valid frame |
| `usb`   | USB host installed? device attached? CDC open? attached VID/PID |

`rtcm_monitor.c` validates **CRC-24Q**, so `valid_frames` means real RTCM3, not
just stray `0xD3` bytes. It's a permanent diagnostic tap — keep it after the
caster lands.

**IDF also ships ready-made USB tools** (no need to reinvent for M0):
- `$IDF_PATH/examples/peripherals/usb/host/usb_host_lib` — attach any device,
  dumps full device/config/interface descriptors. Use this to read the Mosaic's
  VID/PID and find the RTCM-streaming CDC interface index, then fill
  `MOSAIC_VID/PID/CDC_ITF` in `usb_cdc_source.c`.
- `.../usb/host/cdc` — CDC-ACM host open/rx/tx reference.

## Milestones

- **M0 — enumerate.** Attach Mosaic; `new_dev_cb` + `cdc_acm_host_desc_print`
  log VID/PID and every interface. Pin `MOSAIC_VID/PID/CDC_ITF`.
- **M1 — bytes flow.** `rtcm_dump_task` reports throughput and counts `0xD3`
  RTCM3 preambles. *(This skeleton stops here.)*
- **M2 — caster.** Drop in the Zig `ntripcaster` embedded build (lwip backend),
  register a local Source, feed `src.ring` from `rtcm_sink_read()`. Rovers pull.
- **M3 — cloud + UI.** Forward base upstream to the cloud caster; Tab5 MIPI-DSI
  status panel (sources / rover count / RTCM rate).

## Build (on a machine with ESP-IDF 5.4+)

```sh
idf.py set-target esp32p4
idf.py menuconfig      # verify USB host, PSRAM, CONFIG_LWIP_MAX_SOCKETS
idf.py build flash monitor
```

## Related

- `~/ntripcaster` — the Zig caster. Branch `phase8-esp32p4-io-abstraction` adds
  the `io.Stream`/`io.Address` backend seam and `-Dio-backend=posix|lwip`. M2
  needs an `io_lwip.zig` backend plus a local-Source feeder reading `rtcm_sink`.
- `~/rtkserver` — the base-kit product concept (Mosaic-go + M5 + PoE).
