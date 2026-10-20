# CLAUDE.md — SignalSweep CYD

> This file orients Claude Code for this repository. Read it first, then `HANDOFF.md`
> (technical state) and `CREDITS.md`. It carries the full project history from the
> chat sessions that built v1 through v3.20, since Claude Code starts fresh.

---

## CURRENT REPO STATE (read this first)

`main` is the **split architecture**: `shared/` (frozen contract, compiled into both
firmwares), `firmware-cyd/` (control plane + HMI, PlatformIO/Arduino), `firmware-c5/`
(the radio, ESP-IDF >= 5.5, target esp32c5). Read `BUILD_MANIFEST.md` (scope boundary
in section 0 is non-negotiable), `PROTOCOL.md`, then `BRINGUP.md`. Milestone status:
**M1 + M2 are written and compile; neither has run on hardware yet.**

The standalone single-board firmware (v3.21 and earlier, everything described in the
sections below) lives on the **`standalone` branch** / tag `standalone-v3.21`, and its
sources are kept in `firmware-cyd/legacy/` (not built) as porting reference. Where the
sections below say `src/` or `include/`, read `firmware-cyd/legacy/` and
`firmware-cyd/include/`. Build commands: CYD `cd firmware-cyd && python -m platformio
run`; C5 `. $HOME\esp\esp-idf\export.ps1; cd firmware-c5; idf.py build`.

---

## What this project is

A touchscreen port and expansion of **SignalSweep** (https://github.com/B0z0dcl0wn/SignalSweep,
GPL-3.0) onto the **ESP32-2432S028R "Cheap Yellow Display" (CYD)** — a ~$15 ESP32
board with a 2.8" 320x240 ILI9341 touchscreen.

SignalSweep is a **passive, receive-only** counter-surveillance scanner: it listens
for BLE and 2.4 GHz Wi-Fi emissions and flags devices matching known surveillance
hardware (ALPR cameras, body cameras, personal trackers, Remote ID drones, Wi-Fi
attack gear). This port adds a tappable UI, per-device deep profiling, and
investigation tools (focus mode, hunt mode, and planned capture/associates views).

**The owner (Paul) values:** verified facts over guesses. Do not invent firmware
versions, pin assignments, download URLs, or library APIs — check the datasheet,
the board JSON, the actual header, or search the web. He has repeatedly (and
rightly) corrected confident guessing. When unsure, say so and verify.

---

## Hardware: the CYD (ESP32-2432S028R)

Single-USB micro-B variant, ILI9341 display, ESP32-WROOM-32 (2.4 GHz Wi-Fi + BLE
only — **no 5 GHz, no PSRAM**, ~520 KB RAM, 4 MB flash).

### Pin map (all verified on real hardware)
- **Display** ILI9341 on HSPI: MISO=12 MOSI=13 SCLK=14 CS=15 DC=2 RST=-1 BL=21(LEDC ch0)
- **Touch** XPT2046 on VSPI: MOSI=32 MISO=39 CLK=25 CS=33 IRQ=36
- **RGB LED** active-LOW: R=4 G=16 B=17
- **Speaker** SC8002B amp: GPIO 26 (LEDC ch1)
- **LDR**: GPIO 34 (input-only)
- **microSD** on VSPI (shared bus w/ touch): MISO=19 MOSI=23 SCK=18 CS=5

### Hard-won board quirks (DO NOT re-derive these — they cost many hours)
1. **Display rotation is 3** (`setRotation(3)`), landscape 320x240, antenna-right.
2. **COLOUR FIX (critical):** this panel has swapped R/B channels at rotation 3.
   The confirmed fix, found via an on-screen 4-mode boot test, is **MODE 2**:
   after `setRotation(3)`, issue `writecommand(0x36); writedata(0xE8);` then
   `invertDisplay(true)`. Without this, red shows as cyan and green as magenta.
   `setSwapBytes()` does NOT fix it (it only affects pushImage/pushColors).
3. **Touch: BOTH axes inverted.** `ty = map(rx, X_MAX, X_MIN, 0, 239)`,
   `tx = map(ry, Y_MAX, Y_MIN, 0, 319)`, then clamp. Calibration (measured):
   X_MIN=286 X_MAX=3823 Y_MIN=174 Y_MAX=3784.
4. **Wi-Fi + BLE cannot run simultaneously** on this ESP32 under Arduino — it
   aborts. They are **time-sliced**: 6 s BLE, 6 s WiFi, alternating. Focus mode
   (v3.19) parks the radio on one protocol instead of slicing.
5. **User_Setup.h** uses `ILI9341_2_DRIVER` (not `ILI9341_DRIVER`), explicit
   `TFT_MISO=12`, `USE_HSPI_PORT`. It MUST be copied into
   `.pio/libdeps/*/TFT_eSPI/User_Setup.h`; a PlatformIO pre-build hook
   (`copy_setup_pre.py`, `extra_scripts = pre:`) does this automatically, because
   PlatformIO overwrites it when it refreshes the lib cache.
6. CYD **clones vary** — display driver (ILI9341 vs ST7789) and inversion differ.
   The 2-USB (USB-C) variant needs a different User_Setup.h (see witnessmenow repo).

### CYD community references (point users here)
- Main hub: https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display
- 2-USB User_Setup.h: https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/DisplayConfig/CYD2USB/User_Setup.h

---

## Architecture

- **Dual-core FreeRTOS:** scan task on **core 0**, UI task on **core 1**.
  Shared `DetectionRegistry g_reg` guarded by `g_reg_mutex`. Alerts flow scan->UI
  via `g_queue`.
- **Direct TFT rendering** (no sprites — not enough RAM). UI copies the registry
  to a `static` snapshot each frame (it's ~10 KB; keep it off the stack).
- **Inline XPT2046 driver** in `include/touch.h` (external lib had API mismatches).
- **Scope:** `uint8_t g_scope[2][320]` double-buffered. WiFi fills a tapered hump
  per channel at `((ch-1)*319)/10` (beacons only, or the scope goes blocky);
  BLE boosts 3 peaks at buffer indices 0/191/319. Draw is bright-top/dim-bottom
  bars, `si=(x*319)/(sw-1)`.
- **Detection engine ported verbatim from upstream** — see `src/signatures.cpp`
  (96 default rules, mechanically diffed identical to upstream) and the scanners.
  Scoring: LIST when summed signals >= 60; ALERT when strongest single >= 70; one
  alert per appearance; entries stale after 2 min.

### File map
```
src/main.cpp         dual-core setup, scan loop (time-slice + focus), alert queue
src/ui.cpp           ALL display + touch: scope, list, detail sub-tabs, hunt, settings
src/scanner_ble.cpp  NimBLE passive scan, rule match, AirTag/FMDN/Tile/Samsung,
                     Flock Penguin, BLE Remote ID, focus deep-collect
src/scanner_wifi.cpp promiscuous mgmt-frame scan, OUI/SSID/IE rules, DJI DroneID,
                     Remote ID (beacon IE + NAN), WiFi security/standard parse
src/signatures.cpp   the 96-rule table + scoring + category->ThreatType mapping
src/focus.cpp        focus profile: WPS, vendor IE, HT streams, advert timing
src/vendor.cpp       IEEE OUI + BT-SIG company/UUID lookups, Apple role decode
src/opendroneid.c    vendored OpenDroneID core (Apache-2.0) — do not edit
src/odid_wifi.c      vendored OpenDroneID Wi-Fi (Apache-2.0) — do not edit
include/*.h          detection.h (data model), focus.h, signatures.h, vendor.h,
                     board_pins.h, touch.h, settings.h, ui.h, scanner_*.h
config/User_Setup.h  CYD TFT_eSPI config (auto-copied by copy_setup_pre.py)
tools/gen_vendor_tables.py  downloads IEEE+BT-SIG data -> include/vendor_db_data.h
```

---

## Build / flash / test

```bash
# optional once, needs internet: full vendor DB (else a small fallback is used)
python tools/gen_vendor_tables.py

pio run --target clean      # clean needed after User_Setup / partition changes
pio run --target upload
pio device monitor --baud 115200
```

- **Partition:** `huge_app.csv` (3 MB app, no OTA) to fit the vendor DB. Check the
  `Flash: [...] xx%` line after builds.
- **Diagnostics:** serial prints `[stats 5s]` scanner counters and `[det] NEW ...`
  per detection. `[focus] ...` when focused. Use these to tell "quiet" from "broken".
- **Field test rule of thumb:** a phone hotspot named `TELLO-TEST01` fires the DJI
  Tello rule within one WiFi slot (~12 s) — a positive test needing no extra gear.
  (iPhone: enable Maximize Compatibility / 2.4 GHz. Android: set hotspot to 2.4 GHz.)

---

## Workflow (git-centric)

**All changes go straight to `main`** — this is a passion/learning project, not a
client deliverable. No feature branches, no PR ceremony. Commit with a clear
message, push. Tag versions that flash cleanly (`git tag vX.Y && git push --tags`)
as fallback points.

**Version discipline:** bump the version string in THREE places every change —
`src/ui.cpp` line 1 + settings footer, `src/main.cpp` line 1 + boot Serial line,
and `platformio.ini` line 1. The on-device version is the one source of truth for
"what's flashed," so keep it in lockstep with commits. (Verify with `grep -n vX.Y`
before committing — a sed once double-bumped to v3.19.1 by mistake.)

Claude Code runs on Paul's machine with real network + git + USB, so it can edit,
commit, push, and `pio run --target upload` directly. (The prior chat-based Claude
had no network and handed over zips/patches — that limitation is gone here.)

**Corporate-network caveat:** Paul's *work* laptop blocks PlatformIO's package
registry and some downloads (Accenture endpoint security). Builds and the vendor
-table download are done on his **personal** laptop. If a download/registry call
fails, that's why — switch machines, don't fight it.

---

## Project history (condensed, oldest first)

1. **SD card browser** (earlier project, same board): first proved out the CYD's
   display + touch + SD. Confirmed landscape orientation and the VSPI/HSPI split.
2. **SignalSweep bring-up:** PlatformIO/cert/library setup; many wrong rotation
   guesses before settling; touch calibration from measured corners; the Wi-Fi/BLE
   coexistence crash solved by time-slicing; architecture settled (dual-core,
   direct render, inline touch, dark oscilloscope theme).
3. **v3.2 handoff** into a fresh chat (the long session hit context limits).
4. **Colour saga:** scope showed purple-not-green. Many failed attempts
   (setSwapBytes, begin() vs init(), manual R/B swapping) before an on-screen
   4-mode boot test identified MODE 2 (MADCTL 0xE8 + invertDisplay) as the fix.
   Lesson: the diagnostic should have come first.
5. **WiFi/BLE scope shape** regressions fixed by restoring v3.2/v3.8 logic
   (beacons-only WiFi feed; per-channel humps).
6. **Touch mirrored** fixed (both axes inverted).
7. **Upstream detection ported** (v3.16): all 96 signatures verbatim, proper
   scoring, AirTag long-form-only (killed false positives from own phone/watch),
   Google FMDN/Tile/Samsung, Flock Penguin, Remote ID. Fixed a real one-beep bug,
   active-scan for names, NimBLE setMaxResults(0) leak, live-registry mute/purge.
8. **Added fingerprints** (v3.17, all sourced): DJI DroneID WiFi IE (26:37:12),
   Tello/ANAFI SSIDs, DJI/Parrot OUIs (corroboration-only), Hak5 00:13:37.
9. **Vendor DB + scrollable info view** (v3.18): IEEE OUI + BT-SIG names, address
   -type analysis, WPS, Apple roles, WiFi security/standard/country, BLE
   services/appearance, full Remote ID fields. huge_app partition. Host-tested.
10. **Focus mode + sub-tabs** (v3.19): tapping a detection STOPS sweeping and works
    that one target (lock WiFi channel / full-duty BLE). Sub-tabs
    INFO|LINKS|TRAFF|RAW|HUNT|CAP. INFO gains WPS/chipset/timing; RAW shows
    advert hex / IE map. LINKS/TRAFF/HUNT/CAP were placeholders.
11. **Hunt mode** (v3.20): HUNT sub-tab — geiger-style walk-down, clicks + pulsing
    on-screen ring + RGB LED all accelerate with RSSI, silent after 8 s. Also
    fixed the speaker LEDC channel that was never initialized (alert beeps had
    been silent all along).

### Confirmed working on hardware through v3.18
Colour, touch, scope, detection pipeline, scrollable info view, all decoded fields
(tested against a known hotspot). v3.19 and v3.20 were host-syntax-checked and
committed but **need on-hardware confirmation** (focus lock, sub-tab nav, hunt
clicks/pulse/LED, and that alert beeps now sound).

---

## Roadmap (agreed with Paul)

Build order, each small enough to flash-test before the next:

- **v3.21 — LINKS + TRAFFIC ("known associates" / conversations).** While focused,
  widen the WiFi filter to mgmt+data; tally per-peer frames/bytes/direction/QoS.
  Show the target's peers (tappable to pivot to them). "Known associates" in three
  labelled tiers: **Observed** (AP a client uses / an AP's clients / networks a
  client probes for / same MAC on BLE+WiFi), **Inferred** (sibling radios from
  consecutive base MACs; devices probing the target AP's SSID), **Correlated**
  (co-travel: devices whose first/last-seen + RSSI track the target over time —
  needs session history, store on SD). BLE pairings/connections are INVISIBLE
  (connections hop channels the ESP32 can't follow) — be honest about that limit.
- **v3.22 — CAPTURE to microSD** as Wireshark-readable **pcapng** (NOT USB —
  SD, because the C5 is coming). Targeted: only frames to/from the focused MAC,
  channel locked. ~16 KB RAM pre-trigger ring so the triggering frames are
  included. Radiotap 802.11 + BLE link-layer encapsulation. Per-packet pcapng
  comment with the matched rule+confidence. Sidecar .txt with the Wireshark
  display filter + ethers-style name map. Truncate data-frame bodies to headers
  (no payloads, no handshakes — deliberate legal line). Also an alerts.csv log.
  Reuse the SD-browser bus setup; route SD writes through the UI task to avoid
  colliding with display SPI.

### Two opt-in features Paul approved (default OFF)
- **Open-network profiling** (headers only): on UNencrypted WiFi, decode addresses,
  protocols/ports, and devices' own announcements (DHCP hostname/vendor-class,
  mDNS, SSDP/UPnP) to profile "the other end." NO payloads, NO DNS queries, NO TLS
  SNI. (Legal reasoning: Joffe v. Google — capturing payload from open networks can
  violate the Wiretap Act; headers/identifiers are safer ground.)
- **BLE interrogation** (deliberate button, never automatic): connect and read the
  standard Device Information service (manufacturer/model/serial/FW rev) + GAP.
  No vendor-specific characteristics.

### Later / hardware-dependent
- **XIAO ESP32-C5** as a UART coprocessor for **5 GHz** coverage (CYD is 2.4-only).
  Feeds detections into the same registry; the "radio" field already distinguishes
  sources. Antenna: U.FL->SMA pigtail + dual-band duck. (C6 is NOT suitable — no
  5 GHz.) UART pins are now LOCKED in `BUILD_MANIFEST.md` §2 (CYD CN1 GPIO22 TX /
  GPIO27 RX; C5 GPIO9 TX / GPIO10 RX). That deliberately uses GPIO22 and 27, so do the
  continuity/pullup checks listed there. Power plan (single LiPo on the C5 BAT pads,
  boost to the CYD) and battery telemetry are also in §2 — see `BUILD_MANIFEST.md`
  and `PROTOCOL.md` for the split architecture. Scanners/profiles already structured so the CYD keeps
  sweeping 2.4 while the C5 handles 5 GHz / focused work.
- Hunt mode could also drive a **Meshtastic relay** over UART to a Heltec node
  (Paul runs a Meshtastic mesh — serial module text mode) to push alerts over mesh.

---

## Conventions

- Keep the vendored OpenDroneID files (`opendroneid.*`, `odid_wifi.*`) unmodified;
  they're Apache-2.0 and self-contained.
- New own-source files get `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Detection rules must be **sourced** and follow upstream's philosophy: a rule that
  fires on ordinary consumer hardware is worse than no rule. OUI-only matches are
  corroboration (weight 30), never a standalone alert, unless the OUI belongs to a
  police/ALPR-only vendor.
- When adding fields, remember the UI copies the whole registry per frame — put
  per-focused-device deep data in the focus profile, not in every Detection.
- Credit upstream prominently in anything user-facing. Star-the-original first.

---

## Working from the backlog

This applies when work starts from a GitHub issue (for example one labeled `claude`). It overrides the "straight to `main`" workflow above for that work.

1. Read the issue and its **Acceptance criteria**. Those are the definition of done.
2. Respect **Out of scope**. Do not expand beyond it. The `BUILD_MANIFEST.md` section 0 scope boundary still applies (no ACTIVE_TRANSMIT handlers).
3. Branch as `claude/<issue-number>-<slug>`. Never commit to `main`.
4. Open a PR with `Closes #<n>` in the description.
5. List anything you could not check under a **Not verified** heading in the PR (for example: needs hardware testing on the CYD or C5).
6. If blocked, comment a specific question on the issue and add the `needs-human` label instead of guessing.
7. Never add the `claude` label yourself, and never touch secrets.
