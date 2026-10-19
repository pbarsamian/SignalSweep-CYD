# M2 bring-up: CYD <-> C5 UART link

What this build does: the CYD boots RX-ONLY, runs the shared-contract self-test,
handshakes with the C5 (HELLO), pushes the mode, then heartbeats GET_STATUS. The C5
answers HELLO / SET_MODE / GET_STATUS for real and returns **canned** scan data
(flagged `"canned":true`). No radio scanning yet (that is M3).

Both firmwares compiled clean (CYD: PlatformIO, flash 10.9%; C5: ESP-IDF 5.5.6,
`esp32c5`, 257 KB image) and **passed every check in section 5 on hardware** on
2026-10-19 (see HANDOFF.md). The CYD also prints a `[link] ...` line every 2 s on its
serial port (frame counters, raw byte count, battery mV, C5 mode): the quickest wiring
triage is `raw=0` (nothing arriving on GPIO27) vs `raw` climbing with `crc` rising
(corrupt) vs `rx` rising (healthy).

## 1. Wire it (bench, no battery yet)

Power each board from its own USB for now. The only connection between them is the
3-wire link; **do not populate the 3.3 V wire**.

| CYD CN1 | C5 pad | Note |
|---|---|---|
| GPIO27 (CYD RX) | D9 / GPIO9 (C5 TX) | crossover |
| GPIO22 (CYD TX) | D10 / GPIO10 (C5 RX) | crossover |
| GND | GND | required |

Before powering: find CN1 pin 1 with a meter and confirm GPIO22 is populated on your
board (BUILD_MANIFEST.md section 2). Record the pigtail colour order in HANDOFF.md.

## 2. Flash the CYD

```
cd firmware-cyd
python -m platformio run --target upload
python -m platformio device monitor --baud 115200
```

The boot line is `[SignalSweep CYD split M2] boot, mode RX-ONLY`, followed by nine
`[selftest]` lines. All nine must PASS (this is the M1 acceptance check, run on the
device). The screen shows the green RX-ONLY pill and `selftest 9/9 PASS`.

## 3. Flash the C5

PowerShell, from the repo root:

```
. $HOME\esp\esp-idf\export.ps1        # once per shell
cd firmware-c5
idf.py -p COMx flash monitor          # COMx = the C5's USB-C port
```

If the port does not appear, hold the C5's BOOT button while plugging in USB-C.
Expected log: `SignalSweep C5 M2 boot, shadow SILENT_RX`, the `link` pin line, and
a `batt` line. Quit the monitor with Ctrl+].

(`idf.py set-target esp32c5` was already run; the `build/` folder is git-ignored.)

## 4. What you should see

Within about 2 s of both boards running:
- CYD screen: `C5 link: UP`, `fw c5-m2`, `C5 shadow: RX-ONLY`.
- C5 log: `HELLO ok, session started, shadow SILENT_RX` then `SET_MODE -> SILENT_RX`.
- Counters tx/rx climbing about once per 2 s; `nack`, `crc`, `timeout` staying at 0.
- Battery shows `n/a` or a voltage (see "Known unknowns").

## 5. Acceptance checks (M2): type these in the CYD serial monitor

The result of each shows on the CYD screen's bottom line and in the serial log.

| Key | Sends | Expected |
|---|---|---|
| `s` | GET_STATUS | `ok`, link stays up |
| `w` | SCAN_WIFI | `ok` (canned, 2 APs) |
| `b` | SCAN_BLE | `ok` (canned, 1 device) |
| `l` | LIST_DETECTIONS | `ERR_UNIMPL` (not wired until M3) |
| `u` | unknown opcode 0x7E | `ERR_OPCODE` |
| `x` | frame with bad CRC | `NACK ERR_CRC` |
| `v` | frame with wrong version | `NACK ERR_VERSION` |
| `g` | GATT_CONNECT_READ, no token | `ERR_MODE` (RX-ONLY refuses ACTIVE ops) |
| `a` | TX_SLOT_A, no token | `ERR_MODE` |
| `h` | HELLO again | `HELLO ok` (new session secret) |

Failure-direction checks:
- **Unplug the CYD's TX wire** (or reset the CYD) for more than 10 s, then reconnect:
  the C5 logs `mode shadow went stale -> SILENT_RX, session dropped`, and the CYD
  re-handshakes on its own.
- **Reset the C5** while the CYD runs: the CYD notices (status `valid:false` or
  missed heartbeats), shows NO LINK, then re-HELLOs and recovers.

The dev console cannot change the mode or mint tokens. Entering TX_DETECTABLE needs the
warning pop-up + touch-confirm (M4), and tokens need the consent flow (M6).

## 6. Known unknowns (tell me what you observe)

- **Battery reading.** It uses GPIO26 to enable the divider and GPIO6 (ADC1_CH5) to
  read it. Those roles come from Seeed's schematic labels and are unverified. With no
  battery attached the value may read `n/a`, 0, or something odd. Set
  `SS_BATT_SENSE 0` in `firmware-c5/main/battery.h` to turn it off.
- **Baud.** Bring-up runs at 115200. Moving to 921600 is not wired yet (no baud
  negotiation); the constant exists in `shared/ss_protocol.h`.
- **GPIO22 / GPIO27** on the CYD: confirm continuity and that GPIO27's pullup does not
  disturb the RX idle level (the link would show no frames if it does).
- **Python 3.14** was used for the IDF install on the dev machine and worked, but
  it is newer than what Espressif documents. If `idf.py` misbehaves on your other
  laptop, that is the first suspect.
