# tab5-caster

NTRIP caster firmware for **M5Stack Tab5 (ESP32-P4)**, fed by a **Septentrio
mosaic-go (mosaic-G5)** base receiver over **USB (CDC-ACM)**.

Sellable, flash-and-go RTK base box: Mosaic + Tab5 in one USB-C cable, no jumper
wiring. Runs a local NTRIP caster so rovers (auto-steer tractors, RTK drones)
keep getting corrections **even when the internet is down** — the offline-autonomy
reason the box exists. FKP / network-RTK synthesis is done in the **cloud**, not
on this box.

> **Status: gold-standard + turnkey verified on real hardware (2026-08).**
> A cold P4 boot self-resets the onboard C6, joins WiFi, auto-starts the caster,
> and a network NTRIP client pulls live multi-constellation RTCM3 (MSM7 + 1006/
> 1033/1230) straight from an attached Mosaic — no operator interaction, no
> external tools. Builds for `esp32p4` on ESP-IDF **5.4.4**.

## Why USB and not UART

UART (3.3V TTL, 3 wires) is the technically shortest path and needs almost no
firmware. USB was chosen anyway because a single USB-C cable is the right
**product** experience for a flash-and-go kit. The cost is this driver: the P4
must act as a USB **Host** and speak CDC-ACM to the Mosaic's composite device.

## Architecture

```
 Mosaic-go ── USB-A (P4 = HS-OTG Host, CDC-ACM) ──┐
                                                   ▼
 usb_cdc_source.c  (sweep 3 COMs, CRC-latch the one streaming RTCM3)
        │  data_cb
        ▼
 rtcm_sink (StreamBuffer)
        │
        ▼
 rtcm_feed_task  ── the ONE reader ──┬─► rtcm_monitor  (CRC-24Q tally: stats/rtcm)
                                     └─► rtcm_caster_push ─► caster tee buffer
                                                                 │
 Zig ntripcaster (embedded, lwip backend) ── rtcm_caster_read ──┘
        │  serves :2101 /MOSAIC
        ▼
 C6 WiFi (ESP-Hosted/SDIO) ── rover pulls RTCM3 over the LAN
```

The **StreamBuffer seam** (`rtcm_sink`) decouples the USB byte source from the
caster: the USB driver's only contract is "push RTCM3 bytes." Because the monitor
and the caster would otherwise both read the same StreamBuffer (and race), a
single `rtcm_feed_task` is the **only** reader — it tees the bytes to the monitor
and to a second buffer the caster drains.

**Boot order matters (offline autonomy).** `app_main` brings up the USB→caster
core *before* WiFi, and WiFi runs on its own task. The C6 radio is a coprocessor
reached over SDIO; `esp_wifi_init()` blocks (retrying the ESP-Hosted transport)
if the C6 doesn't answer, so gating the caster behind it would let a WiFi-only
fault brick the box — exactly the opposite of the offline-autonomy promise.

## The three things that will bite (read before hacking)

1. **The RTCM COM is discovered, not fixed.** On the bench the Mosaic enumerates
   as **3 CDC-ACM COMs (itf 0/1, 2/3, 4/5) + a USB mass-storage interface (itf6)**
   — *no* ECM/RNDIS network interface. Which COM carries RTCM3 is a receiver-config
   choice (itf0 was silent, itf4 was an NMEA decoy, itf2 was RTCM3), so the driver
   **sweeps the three comm interfaces and latches the one whose bytes pass CRC-24Q**
   (NMEA is auto-skipped). The box also **self-provisions the RTCM3 output** on
   boot (see *Self-provisioning* below), so you no longer have to configure the
   Mosaic by hand — but note only **USB1 (itf2)** answers commands; the first COM
   (itf0) is silent both ways, so provisioning is attempted per-swept-interface
   until one acks.
2. **USB-A VBUS is gated by an I/O expander.** The 5V rail on the USB-A host port
   is switched by PI4IOE5V6408 #2 (I²C `0x44`, P3 = USB5V_EN, active-high) — see
   `board_power.c`. Without driving it the Mosaic is only trickle-powered and its
   D+ pull-up never rises, so it never enumerates. **Brownout is real:** the panel
   backlight shares this rail, and at full brightness a soft PC-USB supply + Mosaic
   streaming load sags it enough to drop the receiver off the bus (`CDC error`, no
   RTCM3). `backlight.c` mitigates this by holding brightness low until the stream
   is up and only climbing as far as the supply sustains (see M3-C) — but a stiff
   5V/2A source is still the right answer in the field.
3. **CDC-ACM host is callback-driven, not a POSIX fd** — hence the StreamBuffer
   instead of wrapping it as an `io.Stream` directly on the USB side.

## Self-provisioning the Mosaic (flash-and-go)

`mosaic_config.c` pushes the box's canonical RTCM3 base-station output config to
the receiver on every boot, so a Mosaic with *any* saved state (even factory)
starts streaming what the caster/FKP need — no operator config step. It sends one
Septentrio command over the CDC command channel:

```
setRTCMv3Output, USB1, RTCM1006+RTCM1033+RTCM1230
                     + RTCM1077+RTCM1087+RTCM1097+RTCM1107+RTCM1117+RTCM1127+RTCM1137   (MSM7, all GNSS)
                     + RTCM1019+RTCM1020+RTCM1042+RTCM1044+RTCM1046                      (eph: GPS/GLO/BDS/QZSS/GAL)
```

Design notes, all verified on hardware via the `mosaic` passthrough:

- **Current config only, never boot config.** Applied to the receiver's RAM, not
  saved with `exeCopyConfigFile` — the box stays the source of truth and the
  Mosaic's NVM (and its flash endurance) is left alone. The box re-applies each
  power-up.
- **Sent per-swept-interface until one acks.** `USB1` (itf2) accepts commands and
  acks with `$R:`; itf0 is silent both ways (returns nothing → `ESP_ERR_TIMEOUT`
  → retry on the next interface). The command targets `USB1` *by name*, so it
  applies regardless of which COM carried it.
- **Longer dwell right after a successful provision.** `setRTCMv3Output` restarts
  the port's output; the stream reappears ~2.5 s later. The interface we just
  acked on is almost certainly the data port, so we dwell 15 s (not 6 s) to catch
  it instead of hopping away and circling back a full sweep.
- **Best-effort, with fallback.** If provisioning fails, the box falls back to
  whatever the Mosaic was already configured to stream. It never bricks the link.

Verify with `rtcm`: after a cold boot the histogram shows MSM7 for every
constellation plus `1019/1020/1042/1044/1046` — with no manual Mosaic setup.

## WiFi via the onboard ESP32-C6 (ESP-Hosted)

The P4 has no radio; the onboard **C6** does WiFi, reached over **SDIO** with
`esp_hosted` + `esp_wifi_remote` (the C6 runs a version-matched ESP-Hosted slave
firmware). Provisioning is over the USB console — `wifiset <ssid> [pass]` saves to
NVS and reboots; on `GOT_IP` the caster auto-starts. (SoftAP/BLE field
provisioning is a `TODO`: SoftAP over ESP-Hosted is an upstream-broken area.)

**C6 reset is self-contained — no external tool needed.** The P4 resets the C6
via **GPIO15** (ESP-Hosted, active-low). The C6's **IO9 boot strap is not wired to
any P4 GPIO**, but the board holds it high with a **10K pull-up (R91 → WLAN_3.3V)**,
so a bare GPIO15 EN pulse always boots the C6 *application* (SDIO slave). A cold P4
boot therefore brings the C6 up on its own.

> **Product note — leave the C6_ISP header (J1) unpopulated in the field.** J1
> breaks out C6 EN (pin3) and IO9 (pin5). An M5 ESP32 Downloader left plugged in,
> even with its port idle, drags IO9 low via DTR/RTS (overriding the weak R91) and
> forces the C6 into download mode on every reset — the SDIO card enumerates but
> the ESP-Hosted transport never connects (`Not able to connect with slave`). This
> dev-rig artifact, not any firmware defect, was behind the earlier "C6 needs a
> downloader EN-reset" episodes. Unplug the downloader and GPIO15 self-reset works.

## Debugging

On-device REPL over the console (USB-Serial-JTAG, same USB-C port as
`idf.py monitor`). Type `help`; commands:

| cmd | what |
|-----|------|
| `stats` | byte count, CRC-valid frame count, CRC fails, ms since last good frame |
| `rtcm`  | monitor's message-type histogram — shows obs (`1005/1077/1087/1097`) + ephemeris (`1019/1020/1042/1046`) presence |
| `dump [n]` / `raw` | hexdump of the last CRC-valid frame / last raw bytes (spot NMEA decoys) |
| `usb`   | USB host installed? device attached? CDC open? attached VID/PID + per-itf topology |
| `mosaic <cmd>` | send a raw Septentrio command to the Mosaic + print the reply (e.g. `mosaic getRTCMv3Output`) — how the provisioning syntax was verified live |
| `caster` | start the Zig ntripcaster (listener + local source) manually |
| `csource` | caster's `/MOSAIC` source state (bytes/types via the tee) |
| `wifiset <ssid> [pass]` / `wifireset` | set / erase WiFi creds in NVS (reboots) |

The periodic 5s log line folds USB state (`usb[host= attach= open= VID:PID
stream=…]`) + rx counters into `idf.py monitor`, so the box is diagnosable from
any capture window without catching one-shot boot events.

`rtcm_monitor.c` validates **CRC-24Q**, so `valid_frames` means real RTCM3, not
stray `0xD3` bytes. It's a permanent diagnostic tap — keep it after the caster.

## Milestones

- **M0 — enumerate. ✅** Mosaic attaches (`152A:85C0`), VBUS gate driven, per-itf
  topology logged.
- **M1 — bytes flow. ✅** Sweep + CRC-latch itf2; CRC-valid multi-GNSS RTCM3.
- **M2 — caster. ✅** Zig `ntripcaster` embedded (lwip backend) fed from the sink
  tee; `/MOSAIC` source ingests live RTCM3.
- **M2.5 — turnkey. ✅** C6 WiFi self-reset on cold boot → STA join → caster
  auto-start → network NTRIP client pulls live RTCM3 end-to-end, no operator input.
- **M3-A — cloud upstream. ✅** Outbound NTRIP SOURCE push of the base RTCM3 to
  the cloud caster (`/TAB5`), exp-backoff reconnect; `upstreamset` creds in NVS.
- **M3-eph — ephemeris + self-provisioning. ✅** The box configures the Mosaic's
  RTCM3 output itself on boot (MSM7 + 1006/1033/1230 + eph 1019/1020/1042/1044/
  1046), verified from a no-eph receiver back to full eph with no manual setup.
- **M3-B — UI. ✅** Tab5 MIPI-DSI status panel (720x1280 ST7123/ILI9881C
  auto-detect) with an LVGL status page — caster/rover count, RTCM rate + CRC +
  constellations, WiFi/IP, cloud upstream — refreshed 1 Hz. De-gated: a panel or
  UI fault can't stall the caster.
- **M3-C — power-aware panel. ✅** The panel LED string and the Mosaic's USB-A 5V
  share a supply, so a fixed-brightness backlight browned the receiver off the
  bus (no RTCM3) on a thin supply — a latent M3-B regression. `backlight.c` now
  holds a safe floor until RTCM3 streams, then climbs toward target only as the
  supply proves it can sustain it, learning a session ceiling on any real (stale)
  stream drop — auto-calibrating, no hard cap. Plus idle-off: the ST7123 touch
  (@0x55, 10 Hz activity poll) blanks the panel after 3 min untouched and wakes
  it on a tap; the RTCM3 stream keeps flowing at 0% backlight while blanked.

## Discovery (mDNS)

`net_mdns.c` advertises the box as **`rtk.local`** with `_ntrip._tcp` (caster) and
`_http._tcp` (status UI), so a field rover / laptop finds it by name. It becomes
reachable once the C6 netif is up. Per-unit unique hostnames (`rtk-<serial>`) are
a product TODO to avoid `.local` collisions on a shared LAN.

## Build (on a machine with ESP-IDF 5.4+)

```sh
idf.py set-target esp32p4
idf.py build flash monitor      # flash over the USB-C port (USB-Serial-JTAG)
```

The Zig caster is cross-compiled into `components/ntripcaster/` by the IDF build
(`zig build caster-lib -Dio-backend=lwip …`); its CMake rule re-runs when any
`~/ntripcaster/src/*.zig` changes.

## Related

- `~/ntripcaster` — the Zig caster. Branch `phase8-esp32p4-io-abstraction` adds
  the `io.Stream`/`io.Address` + `os.*` thread/sync backend seam and
  `-Dio-backend=posix|lwip`, plus the `embedded.zig` C-ABI entry the firmware links.
- `~/rtkserver` — the base-kit product concept (Mosaic-go + M5 + PoE).
