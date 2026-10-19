# SignalSweep-CYD — Build Manifest & Handoff

**For:** Claude Code (or any agent) picking up this build.
**Read order:** this file → `PROTOCOL.md` → the four shared headers. Then build.

This is a **three-tier** evolution of `pbarsamian/SignalSweep-CYD`. The radio work
moves off the CYD onto a XIAO ESP32-C5; the CYD becomes a control plane + HMI
running an access point and a tiny MCP server; a laptop (Ollama/Qwen or Claude
Code) drives operations over MCP. The pattern mirrors `pbarsamian/ai-for-dragons`
(dragon-box): an MCP server bridging an LLM to hardware tools, with hardware-
exclusivity management.

---

## 0. Scope boundary — READ FIRST, NON-NEGOTIABLE

Build the **entire framework**: UART link, MCP server, AP, capability registry,
token-gated consent flow, mode state machine, UI, and **all PASSIVE + ACTIVE_CONNECT
(GATT) handlers** on the C5.

Do **NOT** author the ACTIVE_TRANSMIT handlers. Opcodes `SS_OP_TX_SLOT_A..D` are
**typed, gated, empty slots**. The framework must route to them, gate them, and
require a human-confirmed token for them — but leave the handler body returning
`SS_ERR_UNIMPL`. Do not implement deauth, evil-twin/captive-portal credential
capture, beacon/BLE spam, RF jamming, or Remote ID spoof-broadcast, in this repo
or any helper. A disclaimer in the UI does not change what the code does. The
owner wires their own or a GPL-compatible module behind a slot later; the consent
architecture treats a user-supplied handler identically.

If a task seems to require writing a transmit payload to proceed, stop and flag it
rather than reframing it as something benign.

---

## 1. Repository layout

```
SignalSweep-CYD/
├── shared/                      # the FROZEN contract — identical bytes on both boards
│   ├── ss_protocol.h            # [PROVIDED] UART frame, opcodes, errors, modes, CRC16
│   ├── ss_capabilities.h        # [PROVIDED] capability registry (X-macro list)
│   ├── ss_token.h               # [PROVIDED] consent-token mint/verify interface
│   ├── ss_capabilities.c        # [TODO] expands SS_CAP_LIST into the table + ss_cap_find/check
│   ├── ss_token.c               # [TODO] HMAC-SHA256 mint/verify (mbedTLS), nonce window
│   └── ss_cbor.{h,c}            # [TODO] minimal CBOR encode/decode for payload args/results
│
├── firmware-cyd/                # control plane + HMI (extends the existing PlatformIO project)
│   ├── platformio.ini           # [MODIFY] keep existing env; add shared/ to build_src
│   ├── copy_setup_pre.py        # [KEEP] existing pre-build hook for User_Setup.h
│   ├── config/User_Setup.h      # [KEEP] confirmed-working TFT_eSPI config (MADCTL 0xE8 + invert)
│   └── src/
│       ├── main.cpp             # [REWRITE] boot → SILENT_RX, bring up link/AP/MCP/UI
│       ├── uart_link.{h,cpp}    # [TODO] framing master: send REQ, match RESP by seq, rx EVENTs
│       ├── mcp_server.{h,cpp}   # [TODO] JSON-RPC: initialize, tools/list (from registry), tools/call
│       ├── softap.{h,cpp}       # [TODO] AP the laptop joins; binds the MCP HTTP/WS server
│       ├── consent.{h,cpp}      # [TODO] per-op disclaimer pop-up → touch-confirm → ss_token_mint
│       ├── mode.{h,cpp}         # [TODO] authoritative mode state machine (see §4)
│       └── ui/
│           ├── mode_pill.{h,cpp}   # [TODO] persistent RX-ONLY / TX-DETECTABLE indicator
│           ├── settings_page.{h,cpp}# [TODO] pill switch + mode-change warning pop-up
│           ├── scope_view.{h,cpp}   # [PORT] live spectrum scope (from existing repo)
│           ├── detection_list.{h,cpp}#[PORT] scrollable detections (now fed from C5 events)
│           └── device_view.{h,cpp}  # [PORT] per-device profile view
│
├── firmware-c5/                 # the radio — NEW *ESP-IDF* project (XIAO ESP32-C5), NOT Arduino
│   ├── CMakeLists.txt           # [TODO] top-level IDF project; pulls in ../shared as a component
│   ├── sdkconfig.defaults       # [TODO] target esp32c5; enable BT/BLE, Wi-Fi, set coex options
│   ├── platformio.ini           # [TODO] OPTIONAL wrapper: platform=espressif32, framework=espidf,
│   │                            #        board=esp32-c5-devkitc-1 (verify id). Or build with idf.py directly.
│   └── main/                    # IDF component layout (not src/)
│       ├── CMakeLists.txt       # [TODO] register main component + shared/ include path
│       ├── main.c               # [TODO] app_main(): UART slave loop; boot shadow = SILENT_RX
│       ├── uart_slave.{h,c}     # [TODO] framing slave: parse REQ, dispatch, emit RESP/EVENT
│       ├── dispatch.{h,c}       # [TODO] opcode → handler; pre-checks ss_cap_check + token + shadow
│       ├── radio_wifi.{h,c}     # [TODO] esp_wifi passive scan; esp_wifi_set_band_mode() for 2.4/5 GHz
│       ├── radio_ble.{h,c}      # [TODO] NimBLE passive scan (NO scan-requests) + GATT connect/read
│       ├── remote_id.{h,c}      # [PORT] ASTM F3411 decode (reuse opendroneid core from upstream)
│       ├── signatures.{h,c}     # [PORT] detection/signature engine from SignalSweep upstream
│       ├── scan_sched.{h,c}     # [TODO] single-radio time-slice scheduler (see §2 + M3) — the hard part
│       └── tx_slots.{h,c}       # [STUB ONLY] all four slots return SS_ERR_UNIMPL — see §0
│
├── tools/
│   ├── gen_vendor_tables.py     # [KEEP] IEEE OUI + BT SIG vendor DB generator
│   └── host_tests/              # [TODO] host-compiled unit tests (see §6)
│
├── PROTOCOL.md                  # [PROVIDED] the wire contract, prose
├── BUILD_MANIFEST.md            # this file
├── CLAUDE.md                    # [UPDATE] point Claude Code at this manifest + §0 boundary
├── HANDOFF.md                   # [KEEP/UPDATE] existing board/color notes
└── CREDITS.md                   # [UPDATE] add opendroneid, upstream SignalSweep, CYM reference
```

`[PROVIDED]` = already written, treat as frozen contract. `[TODO]` = build it.
`[PORT]` = adapt existing SignalSweep-CYD / upstream code. `[STUB ONLY]` = §0.

---

## 2. Hardware facts (verify before wiring — do not trust from memory)

**CYD — ESP32-2432S028R, single-USB micro-B / ILI9341 variant.** Known-occupied:
ILI9341 on HSPI; XPT2046 touch + microSD on VSPI; RGB LED on GPIO 4/16/17
(active-LOW); speaker (SC8002B) on GPIO 26; LDR on GPIO 34. Display config is the
confirmed `config/User_Setup.h` (this board needed **MADCTL 0xE8 + invertDisplay(true)**;
keep it).

**UART link — LOCKED pin map (use these exact pins in both firmwares).** The link
uses the CYD's **CN1** connector (the 4-pin PicoBlade header carrying 3.3 V + two
free GPIOs + ground) and the XIAO C5's **GND / D9 / D10** edge pads. (CLAUDE.md
previously said to avoid GPIO22 and note the GPIO27 pullup; these two pins are now
a deliberate choice, so verify the checks below rather than re-deriving.)

| 4-wire pigtail | CYD · CN1 | role | ↔ | XIAO C5 pad | C5 GPIO | role |
|---|---|---|---|---|---|---|
| red — **leave unpopulated** | 3.3V | not used (see Power) | — | 3V3 | — | not used |
| gold / yellow | GPIO27 | **CYD RX** | ← | D9 | **GPIO9** | **C5 TX** |
| blue | GPIO22 | **CYD TX** | → | D10 | **GPIO10** | **C5 RX** |
| black | GND | ground | — | GND | — | ground |

- **CYD side (Arduino):** `Serial2` (UART2) with `Serial2.begin(baud, SERIAL_8N1,
  /*RX=*/27, /*TX=*/22)`. UART0 (GPIO1/3) stays free for flashing + serial monitor.
- **C5 side (ESP-IDF):** a hardware UART (e.g. `UART_NUM_1`) routed via the GPIO
  matrix: `uart_set_pin(UART_NUM_1, 9, 10, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)`
  (TX=GPIO9, RX=GPIO10). The C5 console/flashing is on native USB-Serial-JTAG
  (USB-C), so this UART does not conflict with logging.
- **The crossover is mandatory:** CYD TX(22) → C5 RX(GPIO10); C5 TX(GPIO9) → CYD
  RX(27). Straight-through wiring will not link.
- **Levels:** both boards are 3.3 V logic — connect directly, no level shifter.
- **Baud:** start 115200 for bring-up, move to 921600 once framing is clean.

**Three look-alike 4-pin PicoBlade sockets on the CYD** (witnessmenow PINS.md; it gives no
physical location): **CN1** = GND, IO22, IO27, 3.3 V (use this one); **P3** = GND, IO35,
IO22, IO21 (no IO27; IO21 is the backlight); **P1** = VIN, TX, RX, GND (serial; carries
VIN, so never put the red wire here). Plugging the pigtail into P3 gives exactly a
one-way link (CYD TX on IO22 works, the C5-to-CYD direction has no IO27). That mistake
was made on the first bring-up (2026-10-19); the link came up once the pigtail was moved
to CN1. **On the owner's board CN1 is the socket right next to the RGB LED**
(observed by the owner; board revisions may differ, so still confirm with a meter).
Identify CN1 with a meter: one end pin is a steady 3.3 V, the opposite end is GND.

Still verify on the physical boards before soldering: CN1 pin-1 end with a meter
(some early CYD revisions didn't populate GPIO22 on CN1 pin 3), the pigtail's actual
color order, and the C5 pad silkscreen. On the C5 left edge the order is
`5V, GND, 3V3, D8, D9, D10` — D8 sits between 3V3 and D9.

### Power — single LiPo, no extra USB, no separate charger board

Source: Seeed XIAO ESP32-C5 schematic **Rev V1.1 (2025-12-25)**, text extracted from
the PDF, plus the Seeed wiki. **Check the revision marking on your board and
measure with a meter before relying on any of it.**

What the C5 board provides:
- **Charger:** SGM40567-4.2XG (4.2 V, single-cell LiPo). The schematic notes
  `ICharge = 24000/120K = 200mA` — fixed by a resistor, not changeable in firmware.
  Charge LED blinks while charging; when charged it stays lit ~51 s, then goes off.
- **Power mux:** TPS2116 selects USB (VBUS) vs battery, preferring VBUS when above
  4.33 V. The charger input is on the **VBUS net, which is also the 5V header pad.**
- **3V3 pad is an output** (behind its own TPS22916 load switch). Buck regulator is
  an SGM6029, Imax=1A. Do not feed 3.3 V into it from the CYD.
- **BAT+/BAT− pads** are on the underside; check polarity against your cell's
  connector (reversed battery can destroy the board). No dedicated protection IC
  was found in the extracted schematic text: **use a protected cell.**

Wiring plan:

```
LiPo (+) ──[SWITCH]──┬── C5 BAT+ pad
                     └── boost converter IN+
LiPo (−) ────────────┬── C5 BAT− pad
                     ├── boost converter IN−
                     └── common ground (CYD GND, UART GND)

boost OUT 5V ──► CYD 5 V input        (NEVER the C5 5V pad)
UART: 3 wires only (CYD22→C5 D10, C5 D9→CYD27, GND). Red 3.3 V wire unpopulated.
Charging: plug USB-C into the C5 (switch ON). That is the only USB use.
```

- **Why never the C5 5V pad:** the charger input shares that net, so the boost
  output would charge the battery from the battery — a pointless loop.
- **Splice the boost input at the battery/switch junction**, not through the C5's
  BAT pads: the boost draws roughly 0.5 A at 3.7 V and the pad trace rating is
  unverified.
- **Boost converter:** input range must cover ~3.0–4.2 V; it should have a
  low-voltage cutoff (the C5 board has none that we found).
- **Charging while running:** 200 mA likely cannot keep up with the CYD + C5 radio
  bursts, so charging is slow unless the CYD is idle. Option B (later, if needed):
  a second switch (or the boost module's EN pin) so the CYD side can be off while
  the C5 side stays on for full-rate charging. Bring-up uses one switch.
- **Cell size:** about 400 mAh or more keeps 200 mA at or below 0.5C.
- **Bulk capacitor:** about 470 µF near the C5's supply pins is a reasonable
  precaution for radio bursts (not verified as required).

### Battery telemetry (C5 → CYD)

- **Voltage:** the board divides VBAT with two 100K resistors (schematic: "Battery
  voltage = ADC sampling voltage * 2.0"). The divider is switched by **GPIO26**
  (`GPIO26_ADC_CRL`) and read on **GPIO6 / ADC1_CH5** (net label `D11/A5`). The
  wiki gives an effective 0–3300 mV range. Confirm the pin roles against Seeed's own
  example before coding.
- **Charge state:** the charger status pin appears to drive only the red charge
  LED; no GPIO route was seen, so "charging vs charged" is probably **not**
  readable in firmware. Verify on the schematic before promising it in the UI.
- **Meaning of the number:** read at the C5's BAT net, so it sags under load and
  includes the switch drop — a rough fuel gauge. Suggested low-battery warning
  threshold 3.4 V (to be tuned). The CYD has no battery sensing of its own (it sits
  behind the boost converter), so this is the pack's only gauge.
- **On the wire:** `GET_STATUS` returns `batt_mv`; the C5 also emits a periodic and
  low-battery STATUS event. See PROTOCOL.md §3 and §3a.

**XIAO ESP32-C5.** Single-core RISC-V @240 MHz, 384 KB SRAM, dual-band (2.4 +
5 GHz) Wi-Fi 6 + BLE 5 + 802.15.4 — the reason it owns the radio. **Framework is
ESP-IDF (v5.5+), not Arduino** — locked decision. IDF has the earliest/fullest C5
5 GHz support and exposes the band-mode + coexistence control this project lives
on; the Arduino core sits on top of IDF and lags on new silicon. The `shared/`
headers are plain C and compile unchanged under IDF.

Hard constraints to design around:
- **No simultaneous dual-band.** The C5 is on 2.4 GHz *or* 5 GHz at any instant,
  never both. Switching is an IDF call: `esp_wifi_set_band_mode()` with
  `WIFI_BAND_MODE_2G_ONLY` / `WIFI_BAND_MODE_5G_ONLY` / `WIFI_BAND_MODE_AUTO`.
- **One radio, three jobs.** 2.4 GHz Wi-Fi, 5 GHz Wi-Fi, and BLE all share the
  single radio — this is the CYD coexistence problem you already solved once,
  relocated here. Expect a full dual-band Wi-Fi sweep on the order of ~5 s, so
  `scan_sched` must break scanning up channel-by-channel and yield, and must not
  let BLE starve Wi-Fi under coex (both are real, measured behaviors on this
  chip). This scheduler is the hardest module in the build — treat it as such.
- **Dual-band antenna** required, or 5 GHz detection is pointless (5 GHz uses a
  separate antenna path).
- Pick two free GPIOs for the UART; verify against the XIAO C5 pinout. Confirm the
  IDF target (`esp32c5`) and that your installed IDF (≥5.5) actually exposes the
  5 GHz scan + band-mode APIs — verify the version, don't assume.

---

## 3. Build order (milestones with acceptance checks)

Build bottom-up; each milestone has a check you can run before moving on.

**M1 — Contract compiles on both sides.** Create `ss_capabilities.c`, `ss_token.c`,
`ss_cbor.*`. ✅ *Check:* a host build links `shared/` and prints the registry via
`ss_cap_find` for every opcode; CRC16 matches a known vector.

**M2 — UART link, passive only.** `uart_link` (CYD) + `uart_slave`+`dispatch` (C5).
Wire up `HELLO`, `SET_MODE`, `GET_STATUS`, and `SCAN_BLE`/`SCAN_WIFI` returning
canned data. ✅ *Check:* CYD sends REQ, C5 responds, SEQ matches, bad CRC → NACK,
unknown opcode → `SS_ERR_OPCODE`.

**M3 — Real passive radio on C5.** Port `remote_id`, `signatures`; implement
`radio_wifi` (esp_wifi passive scan + `esp_wifi_set_band_mode()`), `radio_ble`
(NimBLE passive scan), and `scan_sched` — the single-radio time-slicer that
rotates 2.4 GHz / 5 GHz / BLE channel-by-channel with yields so none starves the
others (see §2). Then wire `LIST_DETECTIONS`, `GET_DEVICE_PROFILE`,
`REMOTE_ID_DECODE`, `FOCUS_TARGET`, and `SUBSCRIBE_EVENTS` streaming. ✅ *Check:*
real detections across both bands + BLE stream to the CYD as EVENTs; no probe/
scan requests emitted (confirm with a second sniffer — the device must stay dark
in SILENT_RX); BLE traffic does not stall Wi-Fi detections under load.

**M4 — Mode state machine + UI.** `mode`, `mode_pill`, `settings_page`. Boot in
SILENT_RX; persistent pill always visible; settings pill switch; SILENT→TX raises
the warning pop-up and requires confirm; TX→SILENT is one tap and aborts active
ops + pushes `SET_MODE(SILENT)` to the C5. ✅ *Check:* pill and gate always agree;
power-cycle always returns to SILENT_RX.

**M5 — MCP server + AP.** `softap`, `mcp_server`. `tools/list` generated from the
registry; PASSIVE `tools/call` runs directly. ✅ *Check:* laptop joins AP, Qwen/
Claude Code calls `scan_ble`/`list_detections` and gets results.

**M6 — Consent flow end-to-end (ACTIVE_CONNECT only).** `consent`; wire
`GATT_CONNECT_READ`/`DEVICE_INFO_READ`. Request → `consent_required` → touchscreen
disclaimer → AUTHORIZE → `ss_token_mint` → UART w/ token → C5 `ss_token_verify` →
execute. ✅ *Check:* in SILENT_RX the op is refused with "enable TX mode first";
in TX mode it requires a physical tap; a token replay or a token minted for a
different opcode fails on the C5; token past TTL fails.

**M7 — TX slots gated & empty.** `tx_slots` returns `SS_ERR_UNIMPL`; registry +
consent still route and gate them. ✅ *Check:* calling `tx_slot_a` in TX mode
raises the transmit disclaimer, and after authorize returns `SS_ERR_UNIMPL` —
proving the gate works without any payload existing. (Per §0, it stays empty.)

---

## 4. Invariants Claude Code must not violate

1. **One authoritative mode.** It lives on the CYD. The UI pill and the gate both
   read that same value — never two copies that can diverge.
2. **Boot silent.** Both boards power up in SILENT_RX / shadow SILENT_RX, always.
3. **Panic-to-silent is one tap**, never gated, and aborts in-flight ACTIVE ops.
4. **C5 fails safe.** If the C5's mode shadow is stale/unknown, it refuses ACTIVE
   ops. A lost `SET_MODE` can only ever fail toward silence.
5. **Token per op, human per token.** No token without a physical touch-confirm;
   one token authorizes exactly one op; TTL is short; nonces are single-use.
6. **Registry is the single source** of the tool list AND the gate. Don't hand-
   write a parallel tool list or a per-opcode gate ladder.
7. **PASSIVE means passive.** No probe requests, no scan-requests, no connections
   on any PASSIVE opcode. If it would emit RF, it isn't PASSIVE.
8. **Shared headers are frozen.** Change the contract only by editing `shared/`
   and rebuilding BOTH firmwares; never patch one side's copy.

---

## 5. Toolchain & environment notes

- **PlatformIO.** On the owner's corporate machine `pio` is not on PATH — use
  `python -m platformio run`, `... run --target upload`, `... device monitor`.
- **Clean builds.** When a config/code change doesn't seem to take effect, run
  `... run --target clean` first — build caching has masked changes before.
- **Don't trust board JSON blindly.** Verify `flash_size`/`partitions` in the
  board `*.json` against the actual chip; community definitions have claimed sizes
  they don't compile with.
- **Corporate network.** Endpoint security can block registry/GitHub library
  fetches (SSL/cert policies). If `lib_deps` fails, the personal laptop is the
  fallback for that fetch step, or pin a known-good CDN zip.
- **Two frameworks, one shared contract.** `firmware-cyd/` is Arduino (PlatformIO);
  `firmware-c5/` is ESP-IDF. Both include `../shared`: the CYD via
  `build_src_filter`/`lib_extra_dirs`, the C5 by registering `shared/` as an IDF
  component (its own `CMakeLists.txt` with `idf_component_register`). Contract is
  compiled on both sides, never copied.
- **C5 build (ESP-IDF).** `idf.py set-target esp32c5`, then `idf.py build`,
  `idf.py -p <port> flash monitor`. Requires IDF ≥5.5 for C5 + 5 GHz. The optional
  `platformio.ini` wrapper (`framework = espidf`) is a convenience only — idf.py
  is the source of truth. On the corporate machine, IDF's installer/clone may hit
  the same SSL/registry blocks as PlatformIO; the personal laptop is the fallback
  for the toolchain install step.

---

## 6. Test strategy

- **Host unit tests** (`tools/host_tests/`, native PlatformIO env or plain gcc):
  CRC16 vectors, CBOR round-trip, `ss_cap_check` truth table across both modes ×
  all classes × token-valid/invalid, token mint→verify happy path + expiry +
  wrong-opcode + replay. These need no hardware.
- **Loopback bring-up:** jumper CYD_TX↔CYD_RX to exercise the framer against
  itself before the C5 exists.
- **Mock C5:** a host program speaking the frame protocol over a USB-serial
  adapter, so the CYD's MCP/consent/UI can be developed before C5 radio code lands.
- **Dark check:** with a separate sniffer running, confirm zero emissions from the
  device in SILENT_RX across M3 — this is the test that proves the core claim.

---

## 7. Open decisions (flag, don't guess)

- Is TX a **mode** or also a **physical arming** step (e.g. hold-to-arm) on top of
  the pop-up? Manifest currently treats it as a mode + per-op token; confirm with
  owner before adding an arming gate.
- MCP transport: **HTTP+SSE** (simplest on ESP32) vs **WebSocket** (nicer for the
  event stream). Pick one in M5; don't build both.
- Event delivery to the laptop for a just-authorized op: re-poll vs push over the
  MCP event stream. Decide in M6.
- UART pins are **locked** in §2 (CYD CN1: GPIO22=TX / GPIO27=RX; C5: GPIO9=TX via
  D9 / GPIO10=RX via D10). Only the physical continuity/color checks in §2 remain;
  then record them in `HANDOFF.md`.
- Power plan (§2) assumes one LiPo on the C5 BAT pads with the boost input spliced
  at the battery. Open: one switch vs two (charge rate while running), and whether
  GPIO6/GPIO26 are the battery-sense pins on the board revision you own.
