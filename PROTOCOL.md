# SignalSweep-CYD — Split Architecture Contract (v0.1-draft)

This is the **contract layer**: the one document both firmwares and the laptop
agree on. Nothing in the three tiers is allowed to drift from this.

```
 ┌──────────────┐   MCP / JSON-RPC over    ┌──────────────┐   UART framed    ┌──────────────┐
 │  Laptop      │   HTTP+SSE / WebSocket   │   CYD        │   binary link    │   XIAO C5    │
 │  Ollama/Qwen │ ───────────────────────▶ │  (control    │ ───────────────▶ │  (the radio) │
 │  Claude Code │ ◀─────────────────────── │   plane+HMI) │ ◀─────────────── │              │
 └──────────────┘                          └──────────────┘                  └──────────────┘
        operator brain              AP + MCP + registry + consent         all RF: scan/BLE/GATT/TX
```

**Design rule that drives everything:** the CYD holds the *only* authoritative
copy of the operating mode and mints the *only* tokens that let the C5 do
anything detectable. The laptop can request; only a human touch on the CYD can
authorize. The C5 refuses any gated op whose token it can't verify.

---

## 1. Operating modes

Exactly two modes. The CYD owns this value; the UI pill and the capability gate
both read it from the same place, so they can never disagree.

| Mode | Value | What the C5 is allowed to do | Detectable? |
|------|-------|------------------------------|-------------|
| `MODE_SILENT_RX`      | `0` | PASSIVE only — listen-only Wi-Fi/BLE scan, Remote ID decode. No probe requests, no connections, no transmits. | No — fully dark |
| `MODE_TX_DETECTABLE`  | `1` | PASSIVE + ACTIVE_CONNECT + ACTIVE_TRANSMIT, each still individually token-gated. | Yes |

Transitions:
- `SILENT_RX → TX_DETECTABLE`: requires the **warning pop-up** + explicit
  touch-confirm. This is the only transition that can *increase* exposure.
- `TX_DETECTABLE → SILENT_RX`: immediate, no confirm. Going quieter is always
  safe. On this transition the CYD **aborts any in-flight ACTIVE op** and tells
  the C5 to drop to listen-only. Panic-to-silent must always be one tap.
- On boot the device **always** comes up in `MODE_SILENT_RX`.

The C5 keeps its own shadow copy of the mode (last value the CYD pushed) and
independently refuses ACTIVE ops if its shadow says `SILENT_RX` — defence in
depth, so a lost mode-change message can only ever fail *safe*.

---

## 2. UART frame format

8N1, 921600 baud (fall back to 115200 if you see framing errors on your wiring).
Little-endian. One UART pair + common ground.

```
 byte   0      1      2      3      4       5..6     7 .. 7+LEN-1   last 2
      ┌──────┬──────┬──────┬──────┬───────┬────────┬─────────────┬────────┐
      │ SOF  │ VER  │ TYPE │ SEQ  │ FLAGS │  LEN    │  PAYLOAD     │ CRC16  │
      │ 0xA5 │ 0x01 │      │      │       │ (u16LE) │  (LEN bytes) │ (u16LE)│
      └──────┴──────┴──────┴──────┴───────┴────────┴─────────────┴────────┘
```

- **SOF** `0xA5` — resync marker. A reader that loses sync scans for this byte.
- **VER** protocol version (`0x01`). Mismatched version → `NACK(ERR_VERSION)`.
- **TYPE** — `REQ`, `RESP`, `EVENT`, `ACK`, `NACK` (see §3).
- **SEQ** — rolling 0–255. RESP echoes the REQ's SEQ so the CYD can match them.
  EVENTs use SEQ=0 (unsolicited).
- **FLAGS** — bit0 `HAS_TOKEN` (payload is prefixed with an 8-byte token),
  bit1 `MORE` (multi-frame response continues), bit2 `COMPRESSED` (reserved).
- **LEN** — payload byte count, `0..4096`.
- **PAYLOAD** — see §3.
- **CRC16** — CCITT (poly 0x1021, init 0xFFFF) over VER..end-of-PAYLOAD.

Max frame 4KB keeps it inside a single C5 UART DMA buffer. Large results
(scan dumps, pcap) stream as multiple `MORE`-flagged frames.

### Payload encoding
Args and results are **CBOR**. CBOR maps cleanly to the JSON coming from MCP on
one side and packs tightly for the wire on the other, with tiny decoders on both
MCUs. A REQ payload is:

```
[OPCODE u8][ (if HAS_TOKEN) token 8B ][ CBOR map of args ]
```

A RESP payload is `[STATUS u8][ CBOR map of result ]`.

---

## 3. Message types & opcodes

`TYPE`:

| Name   | Val | Direction      | Meaning |
|--------|-----|----------------|---------|
| REQ    | 1   | CYD → C5       | Invoke an opcode |
| RESP   | 2   | C5 → CYD       | Result for a REQ (matched by SEQ) |
| EVENT  | 3   | C5 → CYD       | Unsolicited (new detection, RSSI tick, TX-started) |
| ACK    | 4   | either         | Bare acknowledge |
| NACK   | 5   | either         | Rejected — payload is `[ERR u8]` |

`OPCODE` (the C5's job list). PASSIVE ops need no token; everything else does.

| Opcode | Name | Class | Notes |
|--------|------|-------|-------|
| 0x01 | `HELLO` | — | handshake, exchanges session secret + versions |
| 0x02 | `SET_MODE` | — | CYD pushes authoritative mode to C5 shadow |
| 0x05 | `SET_CONFIG` | — | detection-type toggles `{drone,tracker,camera,bodycam,attack: bool}` and the scan scheduler `{smode,bpct,wms,bms,bdms}` (§3b-4) and open-network profiling `{onet: bool}` (§3b-3); the CYD pushes its saved settings after every HELLO; a type switched off is dropped from the C5 registry |
| 0x04 | `SET_POWER` | — | user-selected charge mode on/off; refused while forced — see §3c |
| 0x03 | `GET_STATUS` | — | radio state, mode shadow, queue depth, battery (`batt_mv`, `batt_low`) — see §3a |
| 0x10 | `SCAN_WIFI` | PASSIVE | listen-only; no probe requests |
| 0x11 | `SCAN_BLE` | PASSIVE | passive scan, no scan-requests |
| 0x12 | `LIST_DETECTIONS` | PASSIVE | current detection table |
| 0x13 | `GET_DEVICE_PROFILE` | PASSIVE | OUI/company, addr-type, WPS, Continuity, IEs |
| 0x14 | `REMOTE_ID_DECODE` | PASSIVE | ASTM F3411 BLE / beacon / NAN |
| 0x15 | `FOCUS_TARGET` | PASSIVE | lock channel / full-time BLE listen on one target; a lease, see §3b-3 |
| 0x16 | `SUBSCRIBE_EVENTS` | PASSIVE | stream detections/RSSI as EVENTs |
| 0x30 | `GATT_CONNECT_READ` | ACTIVE_CONNECT | initiates a connection → detectable |
| 0x31 | `DEVICE_INFO_READ` | ACTIVE_CONNECT | BLE DIS read |
| 0x50 | `TX_SLOT_A` .. | ACTIVE_TRANSMIT | **registry slots, handler user-supplied — see §5** |

### 3a. EVENT payload and status/battery telemetry (draft)

An EVENT payload is `[KIND u8][ CBOR map ]` (SEQ = 0). Kinds are defined in
`ss_protocol.h` (`ss_event_kind_t`):

| Kind | Val | Body (CBOR map keys) |
|------|-----|----------------------|
| `SS_EVT_DETECTION`  | 1 | new/updated detection (mac, type, name, rssi, channel, radio, ...) |
| `SS_EVT_RSSI_TICK`  | 2 | focused target RSSI (`mac`, `rssi`, `age_ms`) |
| `SS_EVT_TX_STARTED` | 3 | an authorized ACTIVE op began (`op`) |
| `SS_EVT_STATUS`     | 4 | `batt_mv`, `batt_low`, `mode_shadow` — periodic, and immediately on low battery |

`GET_STATUS` result (CBOR map) gains:
- `batt_mv` (u16) — battery millivolts, `0` if unavailable.
- `batt_low` (bool) — true at or below `SS_BATT_LOW_MV` (3400, to be tuned).

Battery source: the C5 board divides VBAT by 2 and reads it on an ADC pin (see
BUILD_MANIFEST.md §2, Battery telemetry). The value is read at the C5's BAT net, so
it sags under load; treat it as a rough fuel gauge. **Charging vs charged is not
reported**: the charger status pin appears to drive only an LED, so no field exists
for it. If that is later verified to be routable to a GPIO, add `charging` here.
The CYD has no battery sensing of its own, so these fields are the pack's only gauge.

### 3b. Control ops, error delivery, liveness (as implemented in M2)

**Where errors go.** Transport-level rejections (bad CRC, bad version, bad length)
are `NACK` frames with payload `[ERR u8]`, echoing the offending frame's SEQ when its
header was readable. Everything that parsed cleanly is answered with a `RESP` whose
first byte is a status: `SS_OK`, or `SS_ERR_OPCODE` (unknown opcode), `SS_ERR_MODE`,
`SS_ERR_TOKEN`, `SS_ERR_UNIMPL` (gate passed, no handler yet), and so on.

**HELLO (0x01)** — args `{ver:u, secret:bytes[32], t_ms:u}`; result
`{ver, fw:text, t_ms, mode}`. The CYD generates a fresh session secret per HELLO. The
C5 stores it, resets its replay window, and **sets its mode shadow to SILENT_RX**
(every new session starts silent). `t_ms` is the CYD's millisecond clock; the C5
keeps `offset = cyd_t_ms - own_ms` and judges token freshness on the CYD clock
(`own_ms + offset`).

**SET_MODE (0x02)** — args `{mode:u}`. Refused with `SS_ERR_MODE` before a HELLO
(no session). The CYD pushes the authoritative mode right after HELLO and on every
change.

**GET_STATUS (0x03)** — result `{mode, valid, up_ms, q, batt_mv, batt_low}`.
`valid:false` means the C5's shadow is not established (reboot, or stale); the CYD
treats that as link-down and re-runs HELLO (the C5 no longer has the secret).

**Liveness.** After HELLO the CYD sends GET_STATUS every `SS_HEARTBEAT_MS` (2 s).
Three missed replies mark the link down. On the C5, if no valid frame arrives for
`SS_SHADOW_STALE_MS` (10 s) the shadow latches **invalid + SILENT_RX**, the session
secret is wiped, and ACTIVE ops are refused until a new HELLO. A lost link can only
fail toward silence.

### 3b-2. Detection data (M3a)

`SCAN_WIFI` result: `{canned:false, total:n, aps:[{mac, ssid, ch, band(2|5), rssi, sec, age_ms}]}`
(access points heard in the last 60 s, freshest first, at most 20). `LIST_DETECTIONS` takes an
optional `{offset:u}` and returns `{total, offset, items:[detection]}` (10 per page). A
`detection` is `{mac, name, type(0 drone,1 tracker,2 camera,3 bodycam,4 attack), conf, best,
rssi, ch, band, radio("wifi"|"ble"|"both"), age_ms, seen, ssid?}`. `SUBSCRIBE_EVENTS` turns on
`SS_EVT_DETECTION` events: the same map plus `new:bool, alert:bool`, sent when a detection is
first listed or first reaches the alert threshold. A new HELLO clears the subscription.
`SCAN_BLE` result: `{canned:false, total:n, devs:[{mac, name, rssi, atype, age_ms, company?}]}`: devices heard in the last 60 s,
the 20 freshest, and `total` is the real count (the C5 tracks up to 64).

### 3b-3. Focus mode (`FOCUS_TARGET` 0x15, PASSIVE)

Focus parks the C5 on one target instead of sweeping. It is passive: Wi-Fi locks the target's
channel and also receives **data-frame headers** (addresses, sizes and QoS flag only; payloads are
never read or stored); BLE scans at full duty (window = interval, still a passive scan, no scan
requests). Nothing transmits. The class is PASSIVE, so it needs no token, but it is refused in
charge mode (radio off) like the scans.

Request `{on:bool, mac:text, radio:"wifi"|"ble", ch:uint}`. `on` defaults to true. `radio` and `ch`
are optional when the MAC is already in the C5's detection list (its own radio and channel are used)
and required for a **pivot**, a MAC the C5 has never matched (a peer). A pivoted MAC is listed as a
low-confidence `Peer (pivot)` entry if there is room; if the list is full, focus proceeds without an
entry. Wi-Fi needs `ch` 1..165, else `SS_ERR_BADFRAME`. Reply `{focus:bool, radio, ch}`. Asking for
the target already held just renews the lease. `{on:false}` releases (the other keys are ignored).

**Focus is a lease.** The CYD renews it (the same request) about every 5 s while INFO shows the
target. The C5 ends it by itself after `FOCUS_TTL_MS` (15 s) without a renewal, on a new HELLO, when
the mode shadow goes stale (link lost), and when the radio is switched off for charge mode. Ending
focus restores the mgmt-only filter, the channel hop and the normal BLE scan duty.

**Focus view**: `GET_DEVICE_PROFILE {view:"focus"}` returns what the parked radio collected, or
`SS_ERR_BUSY` if nothing is focused (the CYD then re-engages).

| key | meaning |
|-----|---------|
| `mac`, `radio`, `ch`, `up_ms`, `age_ms`, `rssi`, `n` | target, how long focused, ms since it was last heard, last RSSI, frames (Wi-Fi) or adverts (BLE) |
| Wi-Fi: `beacons`, `presp`, `preq`, `omgmt`, `data` | frame counts by kind (`data` counts data frames to or from the target) |
| Wi-Fi: `iemap` (32 B), `ht`, `vie` (n x 4 B) | element-ID bitmap, spatial streams, vendor-IE OUI + type |
| Wi-Fi: `peers` | up to 12, busiest first: `{mac, role(0 unknown,1 client of target,2 AP of target), rssi, tof, frf, tob, frb, qos, age_ms}` |
| Wi-Fi: `onet` | only when open-network profiling is on (below): up to 6 devices, busiest first |
| BLE: `advs`, `adv_int`, `raw` | adverts heard, smallest gap in ms, latest advert payload (<= 62 B) |

`SS_EVT_RSSI_TICK` (needs `SUBSCRIBE_EVENTS`), about 5 per second while focused: `{mac, rssi, age_ms,
n}`; `age_ms` 99999 means the target has not been heard yet. The CYD's HUNT page runs on these.

**Frame capture** (issue #10, PASSIVE). `SET_CONFIG {cap: bool}`, reset to off on every HELLO and ended by the focus ending, a
retarget, or `cap:false`. It only takes effect during a focus. While on, every frame to or from the focused Wi-Fi MAC (any of addr1,
addr2, addr3), or every advert from the focused BLE MAC, is queued on the C5 (8 KB ring) and streamed to the CYD as `SS_EVT_CAPTURE` (event kind 5) events, metered to about
8 KB/s so the link keeps room for everything else. Data frames are cut to their 802.11 header at the source (never the LLC, IP or
anything above, so no payloads and no EAPOL handshakes); management frames are kept whole up to 400 bytes. Event body (CBOR):
`{a: capture still on, e: this is the last event of the capture, d: records dropped so far (queue full), r: bytes}` where `r` packs
whole records back to back, little-endian: `u32 t_ms, u8 ch, s8 rssi, u8 kind (0 mgmt, 1 data, 2 BLE advert), u8 flags (bit 0 truncated; kind 2: bit 1 TxAdd,
bit 2 RxAdd, bits 4..7 link-layer PDU type), u16 caplen, u16 origlen (without the FCS), u8 data[caplen]`. For kind 2, `ch` is 0 (the
scan does not report the advertising channel) and `data` is AdvA (6 bytes, over-the-air order), then InitA (6 bytes) for ADV_DIRECT_IND only,
then the advertising data (<= 31 bytes). The CYD rebuilds each advert as a link-layer packet (LINKTYPE_BLUETOOTH_LE_LL 251: access address
0x8E89BED6, header, AdvA and data, CRC = zeros because it is unknown) with the RSSI in the packet comment. A passive scan never receives scan
responses, so none are captured. After `cap:false` or the end of the focus the C5 drains its queue and
sends one final event with `e:true`. Code: `firmware-c5/main/capture.*` and `firmware-cyd/src/sdcap.*`. The CYD writes a pcapng
(radiotap, linktype 127, microsecond timestamps relative to the first frame) to `/signalsweep/cap_NNN.pcapng` with a `.txt` sidecar.

**Open-network profiling** (issue #11, PASSIVE). `SET_CONFIG {onet: bool}`, default off, and the C5 sets it
back to off on every HELLO, so a new session starts off until the CYD (which does not save the setting) asks.
While a Wi-Fi focus is active and it is on, the C5 reads the data frames of the focused target (any frame whose
addr1, addr2 or addr3 is the target) just far enough to learn about the device that sent them. It reads a frame only if
the Protected bit is clear (so never an encrypted frame), it is LLC/SNAP IPv4, and it is the first IP fragment. It keeps:
the sender's IP protocol and TCP/UDP destination-port tallies (headers only); the DHCP client hostname (option 12) and
vendor class (option 60) of a client message; the names a device announces in an mDNS *response* (PTR instance, SRV/A/AAAA
owner; never queries, never TXT); and the SSDP NOTIFY `SERVER` header. It never reads port 53 (DNS) in either
direction, never reads a TCP payload (so no TLS SNI, no HTTP), and no payload bytes are stored: only the short extracted
strings, in RAM, until the focus ends (`opennet_clear`). Each `onet` item: `{mac, tcp, udp, icmp, h?, vc?, nm?[<=3],
sv?[<=2], ss?, pt?[[port, proto, count] x <=4]}`. Code: `firmware-c5/main/opennet.*`.

Limits: BLE connections hop channels and are not visible, so a BLE target has no peers. The
"Correlated" tier of LINKS (co-travel over time) needs session history on SD and is not built.

### 3b-4. Scan scheduler (`SET_CONFIG`)

Wi-Fi capture and the BLE scan share the C5's one 2.4 GHz radio, so running both at once costs each
some frames. The scheduler (`firmware-c5/main/sched.*`) decides the split. It only pauses and
resumes receive paths; nothing transmits.

`SET_CONFIG` takes four optional keys (missing keys leave a value as it was; the CYD sends all four
after every HELLO along with the detection toggles):

| key | meaning |
|-----|---------|
| `smode` | 0 `CONCURRENT`: both run all the time. 1 `SLICED`: the radio alternates, Wi-Fi only then BLE only |
| `bpct` | CONCURRENT: BLE scan window as a percent of its 100 ms interval, clamped 10..90 (Wi-Fi gets the rest) |
| `wms`, `bms` | SLICED: length of the Wi-Fi slot and the BLE slot in ms, clamped 500..30000 |
| `bdms` | Wi-Fi band dwell: scan 2.4 GHz for this long, then 5 GHz, and so on (1000..30000 ms, default 6000). 0 = the old single interleaved sweep. Only time actually spent scanning Wi-Fi counts |

Why the clamp: a 100 % BLE window starves Wi-Fi completely (0 frames measured). Blocking Wi-Fi on
purpose is what the focus lock below is for.

**Lock.** Focus mode takes the radio: a Wi-Fi focus runs Wi-Fi only (BLE paused), a BLE focus runs BLE
only at full duty (Wi-Fi receive paused). Releasing focus hands control back to the configured mode.
Charge mode is separate: the radios stay fully off whatever the scheduler wants.

**Reporting.** `GET_STATUS` and `SS_EVT_STATUS` carry `scan` (1 Wi-Fi only, 2 BLE only, 3 both), `lock`
(0 none, 1 Wi-Fi, 2 BLE) and `band` (2 or 5: the Wi-Fi band the hop has chosen). The C5 pushes a status
event the moment any of them changes, so a screen can follow the radio without waiting for the heartbeat.
In SLICED every Wi-Fi slot stays on one band and the next slot takes the other (so 2.4 GHz and 5 GHz
alternate between BLE slots); in CONCURRENT the band follows the band dwell (`bdms`).

The CYD scope's AUTO setting follows this: Wi-Fi only shows the band it is on, BLE only shows the BLE list,
and when both run together it rotates 2.4 GHz, 5 GHz and BLE every 6 s (a timer on the CYD).

Measured on hardware (per 5 s, 60 s averages, a busy BLE environment; Wi-Fi counts are noisy because the
hop covers 36 channels, and 5 GHz is far busier than 2.4 GHz):

| | `smode` / `bpct` | Wi-Fi mgmt frames | BLE adverts |
|---|---|---|---|
| Before the scheduler (fixed 50 % window, interleaved sweep) | - | 48.6 | 1093 |
| BALANCED (default) | 0 / 50 | 37.6 | 1121 |
| WI-FI | 0 / 20 | 61.2 | 412 |
| BLE | 0 / 80 | 11.5 | 1729 |
| SLICED 4 s Wi-Fi / 2 s BLE | 1 / - | 51.4 | 738 |

Access points found are unchanged by the band dwell (about 2-3 on 2.4 GHz and 27-30 on 5 GHz). BALANCED
reports fewer raw Wi-Fi frames than before because the radio now spends half its Wi-Fi time on the quiet
2.4 GHz band (it used to spend about 31 %); BLE is unaffected. Running both radios at the old 50 % split
cost Wi-Fi about half its frames (SLICED, which gives Wi-Fi the radio alone, runs about 85 frames per
active 5 s).

### 3c. Charge mode (power state)

Orthogonal to the RF mode. In charge mode the **C5 radio is fully off** (promiscuous mode
disabled, Wi-Fi driver stopped, hop task idle) and the **CYD shows only the battery voltage**
(backlight dimmed, CPU slowed to 80 MHz, redraw only on change). The UART link stays up so the
voltage keeps updating; scan opcodes return `SS_ERR_BUSY`.

States (`ss_power_t`): `0 NORMAL`, `1 CHARGE_USER`, `2 CHARGE_FORCED`.

- **The C5 owns the state** because it measures the battery. The CYD mirrors it (the status
  event and `GET_STATUS` carry `power`).
- **Selectable:** the CYD sends `SET_POWER {charge:bool}`. The user can enter and leave
  `CHARGE_USER` freely.
- **Forced:** at or below `SS_BATT_LOW_MV` (3400 mV) the C5 enters `CHARGE_FORCED` from either
  other state. **The 4.0 V hold applies only to this forced state:** it is left only when the
  battery exceeds `SS_BATT_EXIT_MV` (4000 mV). While forced, `SET_POWER` is answered
  `SS_ERR_LOCKED` and the CYD ignores taps. A charge mode the user chose carries no such lock.
- **Boot:** the C5 reads the battery before starting the radio. A pack already at or below the
  threshold boots into `CHARGE_FORCED` with the RF front end never switched on.
- **A reading of 0** (no cell or unusable) never forces charge mode. With no cell attached the
  sense line floats near 2.3 V, which the firmware reports as 0.
- **No USB means no exit:** a forced charge session on battery alone cannot reach 4.0 V, so the
  unit stays in charge mode until it is plugged in. That is the intended behaviour.

**Power savings while charging.** Goal: keep the total draw below the C5 charger's fixed
200 mA so the battery gains charge.
1. **Display fully off.** The CYD sets the backlight to 0 (the panel is deliberately not put into
   sleep: ILI9341 sleep mode shows white on many boards) after `SS_CHARGE_WAKE_MS` (8 s) in charge mode. The touch controller stays on:
   the first tap only wakes the screen for 8 s (and refreshes the voltage); a second tap on the lit
   screen leaves a *user* charge mode, and is ignored in a forced one.
2. **CYD supply cut (optional hardware).** If `SS_BOOST_EN_GPIO` is set and wired, the C5 drives
   the boost converter's EN pin low (open-drain) `SS_CYD_CUT_DELAY_MS` (30 s) after entering a
   charge state, cutting the CYD's power entirely. Status replies and events carry `cut_s`
   (seconds until the cut; 0 = not scheduled) and the charge screen shows the countdown. EN is
   released the moment charge mode ends, so a forced exit (battery above 4.0 V) powers the CYD
   back up and it re-handshakes. With the CYD dark there is no touchscreen, so a *user* charge
   mode is left by pressing the **BOOT button on the C5** (GPIO28); it is ignored while forced.
3. **C5 clock and wake-ups.** CPU at 80 MHz in any charge state (240 MHz otherwise); the link task
   polls every 500 ms instead of 20 ms (arriving bytes still wake it at once); the idle hop task
   sleeps 1 s; the stats log line drops from every 5 s to every 60 s.
4. **Slower link.** CYD heartbeat `SS_CHARGE_HEARTBEAT_MS` (15 s, was 2 s), C5 status event 30 s
   (was 10 s), and the C5's stale window `SS_CHARGE_SHADOW_STALE_MS` (45 s, three heartbeats).
   Every opcode that needs the radio (scans and all ACTIVE ops) is refused with `SS_ERR_BUSY`
   while charging.

**Retry/timeouts.** A REQ unanswered after `SS_REQ_TIMEOUT_MS` (600 ms) is dropped
by the CYD; there is no automatic resend.

---

## 4. Capability registry schema

Every opcode the laptop can reach is described by one declarative entry. The gate
logic falls straight out of the table — there's no per-opcode `if` ladder.

```c
typedef struct {
    uint8_t      opcode;
    const char*  id;           // stable string id, also the MCP tool name
    CapClass     cls;          // PASSIVE | ACTIVE_CONNECT | ACTIVE_TRANSMIT
    bool         requires_token;
    ModeMask     allowed_modes;// bitmask of modes in which this is callable
    const char*  disclaimer;   // shown in the per-op pop-up; NULL if none
} CapEntry;
```

Gate check, in full:

```
callable(entry, mode, token):
    if entry.allowed_modes does not include mode:     reject ERR_MODE
    if entry.requires_token and not valid(token):     reject ERR_TOKEN
    else forward to C5
```

- PASSIVE entries: `requires_token=false`, `allowed_modes = SILENT_RX|TX_DETECTABLE`.
- ACTIVE_CONNECT / ACTIVE_TRANSMIT: `requires_token=true`,
  `allowed_modes = TX_DETECTABLE` only.

---

## 5. Consent flow & tokens

A token is minted by the CYD **only after a physical touch-confirm**, and only
the C5 can validate it (it's bound to the session secret the laptop never sees).

```
laptop/LLM ── tools/call "gatt_connect_read" ──▶ CYD MCP server
                                                  │
                        registry: class=ACTIVE_CONNECT, requires_token
                                                  │
                        mode == TX_DETECTABLE?  ── no ──▶ RESP error: enable TX mode first
                                                  │ yes
                        raise per-op disclaimer pop-up on touchscreen
                                                  │
                        human taps AUTHORIZE ─── no/timeout ──▶ RESP error: denied
                                                  │ yes
                        mint token {cap_id, nonce, issued_ms, ttl_ms=8000, hmac}
                                                  │
                        UART REQ(opcode, FLAGS.HAS_TOKEN, token, args) ──▶ C5
                                                  │
                        C5: verify hmac w/ session secret, ttl not expired,
                            cap_id matches opcode, mode shadow == TX_DETECTABLE
                                                  │ ok
                                              execute, RESP
```

Token struct (wire = 8 bytes after a local expansion; see `ss_token.h`):

```c
typedef struct {
    uint16_t cap_id;     // which capability this authorizes
    uint16_t nonce;      // single-use, C5 tracks a small recent-nonce window
    uint32_t issued_ms;  // CYD clock
    // transmitted MAC truncated to fit; full struct hashed, see header
} TokenCore;
```

Threat model is explicit and narrow: **stop the laptop or a creative model from
self-authorizing a detectable/transmit op.** It is *not* a defence against an
attacker with a logic probe on your UART header — that's your own wire. The HMAC
exists so a token forged or replayed by the laptop (which lacks the session
secret) fails on the C5. Single-use nonces stop replay of a captured token.

---

## 6. MCP surface (CYD server)

MCP is JSON-RPC 2.0. The CYD implements the three methods that matter —
`initialize`, `tools/list`, `tools/call` — over HTTP+SSE (simplest on an ESP32)
or WebSocket. `tools/list` is generated directly from the capability registry,
so the tool surface and the gate can't disagree. Tool names == `CapEntry.id`.

PASSIVE tools return results directly. ACTIVE tools return, on first call, a
`consent_required` result describing the pending touch-confirm — the actual
operation only runs after the human authorizes, and the laptop re-polls or
receives it over the event stream. (Qwen via Ollama and Claude Code both drive
this with normal tool-calling.)

---

## 7. What is *not* in this repo

The ACTIVE_TRANSMIT registry slots (`0x50+`) are **typed, gated, empty slots**.
The framework routes to them, gates them, and demands a human-confirmed token
for them — but the transmit handlers themselves (deauth, evil-twin portal,
beacon/BLE spam, RF jam, Remote ID spoof-broadcast) are **not** provided here.
Those are attack payloads; some are illegal to operate. Wire in your own or a
GPL-compatible module behind the slot. The consent architecture treats a
user-supplied handler and a built-in one identically.
