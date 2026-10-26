# SignalSweep CYD — Handoff for a New LLM / Coding Agent

Written 2026-10-26. Repo state at writing: `main` @ `a67841b`, clean, in sync with `origin/main`.

## 1. Connect to the repo

- **Repo:** https://github.com/pbarsamian/SignalSweep-CYD (owner: `pbarsamian`)
- **Branch:** `main`. Paul commits straight to `main`: no feature branches, no PRs.
- **Other branch:** `standalone` (tag `standalone-v3.21`) holds the old single-board firmware. Its sources are also kept in `firmware-cyd/legacy/`, which is not built and is only a porting reference.
- **Backlog:** GitHub Issues (`gh issue list`).

```bash
git clone https://github.com/pbarsamian/SignalSweep-CYD.git
cd SignalSweep-CYD
git log --oneline | head      # expect a67841b at the top, or newer
gh issue list --limit 30      # needs `gh auth login`
```

Pushing needs Paul's GitHub credentials (`gh auth login`, a PAT, or an SSH key). Ask Paul to set that up. Do not invent or hunt for credentials.

## 2. Read these first, in this order

1. `CLAUDE.md`: project orientation, hardware quirks, conventions, workflow. Much of its file map and roadmap describes the old standalone firmware. Where it says `src/` or `include/`, read `firmware-cyd/legacy/` and `firmware-cyd/include/`.
2. `HANDOFF.md`: technical state and what is **hardware-verified**. Treat it as the source of truth for verification status.
3. `BUILD_MANIFEST.md`: the scope boundary in §0 is **non-negotiable**. UART pins are locked in §2.
4. `PROTOCOL.md`: CYD/C5 wire contract plus the detection-list export schema v1.
5. `BRINGUP.md`, `CREDITS.md`, `README.md`.

## 3. What the project is

A touchscreen port and expansion of the passive, receive-only counter-surveillance scanner SignalSweep (upstream: B0z0dcl0wn/SignalSweep, GPL-3.0).

| Piece | Path | Stack |
|---|---|---|
| Shared contract (compiled into both) | `shared/` | frozen, change deliberately |
| CYD (control plane and touch HMI), ESP32-2432S028R | `firmware-cyd/` | PlatformIO / Arduino |
| XIAO ESP32-C5 (the radio, 2.4 + 5 GHz) | `firmware-c5/` | ESP-IDF >= 5.5, target esp32c5 |
| Tools, hardware docs | `tools/`, `hardware/`, `docs/` | |

## 4. Where development stopped (2026-10-21)

- **#10 Capture to SD (pcapng):** built and hardware-verified. It covers Wi-Fi and BLE pcapng, per-packet rule comments, `alerts.csv` (SET > PRIVACY switch, off at boot) and a 16 KB pre-trigger ring.
  - It is still open only because Paul has not opened a capture in real Wireshark. Try `cap_007.pcapng` (Wi-Fi) and `cap_004.pcapng` (BLE). The sidecar BLE filter `btle.advertising_address` is unchecked.
  - Smaller unchecked items: ring freed on focus release (heap should return to about 42.9 KB), and BLE pre-trigger.
- **#15 List export:** built and verified (LIST > SAVE, schema v1 in `PROTOCOL.md`). Only the MCP read tool is left, and it waits for #9.
- **Latest commit (`a67841b`):** `PARTS.md`-style update adding the reference build's parts list.

**Open issues:** #4 (M4/M6 mode state machine, consent flow, token-gated BLE interrogation), #5 (charge-mode current, CYD supply cut), #9 (M5 MCP server and AP for laptop control), #10, #11 (open-network profiling; HANDOFF says built, so check whether it can be closed), #12 (Meshtastic alert relay over UART), #15.

**Suggested next steps:**
1. Have Paul do the Wireshark check, then close #10.
2. Start #9 (MCP server and AP). **Hold a design discussion with Paul first.** Do not just start building.
3. Then #12.

## 5. Build and flash (Windows, PowerShell)

Ports on Paul's setup: **CYD = COM9** (CH340), **C5 = COM10** (Espressif USB).

```powershell
# C5 (ESP-IDF)
. $HOME\esp\esp-idf\export.ps1
cd firmware-c5
idf.py build
idf.py -p COM10 flash

# CYD (PlatformIO), in a SEPARATE shell call
cd firmware-cyd
python -m platformio run --target upload --upload-port COM9
```

- Run the two builds in **separate shells**. The IDF export swaps in IDF's Python, which has no platformio.
- The CYD USB link has dropped mid-flash before. Retry once after the port reappears, and suggest a different cable.
- The CYD USB is serial only, not mass storage. Console key `d` dumps `alerts.csv` over serial. CYD console keys are in `firmware-cyd/src/main.cpp`, and `a` is taken (TX-slot test).
- The SD card is read on Paul's PC at `D:\signalsweep`. Paul swaps the card.
- **Ask before each flash.** Paul says "flash" each time. Start logging the C5 only after Paul says "focused" or "starting".
- **Work laptop caveat:** the Accenture laptop has flashed fine recently, but older notes say it blocks the PlatformIO registry and some downloads. If a download fails, try the personal laptop.

## 6. Conventions and gotchas

- **Paul values verified facts over guesses.** Never invent pin numbers, versions, URLs or library APIs. Check the header, datasheet or board JSON, or search. Say so when unsure.
- **Line endings:** `radio_wifi.cpp` is LF. `capture.*` and `radio_ble.cpp` are CRLF. Use the Edit tool for source changes. A Python helper once rewrote line endings. Check `git diff --stat` for whole-file diffs.
- New own-source files get `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Do not modify the vendored OpenDroneID files (`opendroneid.*`, `odid_wifi.*`, Apache-2.0).
- Detection rules must be **sourced**. A rule that fires on ordinary consumer hardware is worse than no rule. OUI-only matches are corroboration (weight 30), not standalone alerts.
- **Legal lines, deliberate:** headers only, no payloads, no handshakes, no DNS or TLS SNI. BLE interrogation is a deliberate button press, never automatic.
- Tag builds that flash cleanly (`git tag vX.Y && git push --tags`).
- Hardware quirks (display colour fix, inverted touch axes, Wi-Fi/BLE coexistence) are in `CLAUDE.md`. **Do not re-derive them.**

## 7. Opening move for the new agent

1. Clone, then read the docs in §2.
2. Give Paul a short status taken from `HANDOFF.md`.
3. Ask: Wireshark check first, or start #9 design?
4. Do not redo finished work.
