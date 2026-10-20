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
| 0x05 | `SET_CONFIG` | — | detection-type toggles `{drone,tracker,camera,bodycam,attack: bool}`; the CYD pushes its saved settings after every HELLO; a type switched off is dropped from the C5 registry |
| 0x04 | `SET_POWER` | — | user-selected charge mode on/off; refused while forced — see §3c |
| 0x03 | `GET_STATUS` | — | radio state, mode shadow, queue depth, battery (`batt_mv`, `batt_low`) — see §3a |
| 0x10 | `SCAN_WIFI` | PASSIVE | listen-only; no probe requests |
| 0x11 | `SCAN_BLE` | PASSIVE | passive scan, no scan-requests |
| 0x12 | `LIST_DETECTIONS` | PASSIVE | current detection table |
| 0x13 | `GET_DEVICE_PROFILE` | PASSIVE | OUI/company, addr-type, WPS, Continuity, IEs |
| 0x14 | `REMOTE_ID_DECODE` | PASSIVE | ASTM F3411 BLE / beacon / NAN |
| 0x15 | `FOCUS_TARGET` | PASSIVE | lock channel / full-time BLE listen on one target |
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
`SCAN_BLE` is still canned until the BLE radio lands.

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
1. **Display fully off.** The CYD switches the panel off (ILI9341 `SLPIN` + `DISPOFF`) and the
   backlight to 0 after `SS_CHARGE_WAKE_MS` (8 s) in charge mode. The touch controller stays on:
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
