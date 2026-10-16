# SignalSweep CYD

A touchscreen port and expansion of **[SignalSweep](https://github.com/B0z0dcl0wn/SignalSweep)**
for the **ESP32-2432S028R "Cheap Yellow Display" (CYD)**.

SignalSweep is a passive, receive-only counter-surveillance scanner. It listens
for the Bluetooth LE and 2.4 GHz Wi-Fi emissions of nearby devices and flags ones
that match known surveillance hardware: ALPR cameras, body cameras, personal
trackers, drones broadcasting Remote ID, and common Wi-Fi attack gear. It never
transmits during scanning and never attempts to decrypt anything.

> ### Credit where it's due
> This project is an expansion of **SignalSweep by [B0z0dcl0wn](https://github.com/B0z0dcl0wn/SignalSweep)**,
> whose excellent work is the foundation for everything here — the detection
> philosophy, the carefully-tuned signature set, the Remote ID decoding, and the
> "tell me what you heard and why" approach that avoids false alarms. All of the
> detection signatures in this port are taken directly from that project. If you
> find this useful, please go star the original first.
>
> This port exists to bring that capability to the **Cheap Yellow Display** — a
> ~$15 ESP32 board with a built-in touchscreen — adding a graphical, tappable UI
> and device-profiling features suited to a screen you can hold in your hand.

## What this port adds

- **Touch UI** for the CYD's 320×240 resistive touchscreen: a live spectrum
  scope, a scrollable detection list, and a detailed per-device info view.
- **Focus mode** — tap a detection and the radio stops sweeping to work that one
  target: locking to its Wi-Fi channel or listening full-time on BLE.
- **Deep device profiling** — vendor (IEEE OUI) and Bluetooth company names,
  address-type analysis, WPS maker/model/serial, Apple Continuity roles, BLE
  appearance and services, Wi-Fi security/standard/country, advertising
  intervals, and raw advertisement/IE inspection.
- **Full ASTM F3411 Remote ID** decode for drones (BLE, Wi-Fi beacon, Wi-Fi NAN)
  including operator/pilot location.
- A few **additional 2.4 GHz fingerprints** beyond upstream (sourced; see
  `HANDOFF.md`): DJI DroneID Wi-Fi IE, Tello/ANAFI SSIDs, DJI/Parrot OUIs.

### Planned / in progress
Hunt mode (walk-down signal meter with pulsing screen + LED), a "known
associates" view (what a device is connected to / paired with / probing for),
a traffic/conversations view, and targeted packet capture to microSD as
Wireshark-readable pcapng. A XIAO ESP32-C5 is planned as a UART coprocessor to
add 5 GHz coverage.

## Hardware: the Cheap Yellow Display

This runs on the **ESP32-2432S028R**, nicknamed the "Cheap Yellow Display" by the
community that formed around it. If you're new to the board, these are the places
to go:

- **[witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)**
  — the main community hub: pin-outs, setup, troubleshooting, add-ons, and a
  decision tree for telling CYD variants apart.
- **[Getting started guide](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/Instructions/SetupAndConfiguration.md)**
  — first-time setup and the CH340 USB driver.
- **[TFT_eSPI User_Setup for the 2-USB (USB-C) variant](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/DisplayConfig/CYD2USB/User_Setup.h)**
  — if your board has two USB ports, you may need this display config.

This project targets the **single-USB micro-B variant** with the ILI9341 display.
The confirmed-working `User_Setup.h` for it is in `config/`, and a PlatformIO
pre-build hook copies it into place automatically.

> ⚠️ **CYD boards and clones vary**, especially the display driver (ILI9341 vs
> ST7789) and colour inversion. If your colours look wrong (e.g. red shows as
> blue), see the colour-order note in `HANDOFF.md` — this board needed MADCTL
> `0xE8` + `invertDisplay(true)`.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
# optional: download the full IEEE + Bluetooth SIG vendor database first
python tools/gen_vendor_tables.py

pio run --target clean
pio run --target upload
pio device monitor --baud 115200
```

The build works without the vendor-table step (a small built-in fallback list is
included); vendor names are just more complete once you run it.

## Legal & ethical use

SignalSweep is **receive-only**. It is meant for understanding the radio
environment around you — spotting surveillance devices, auditing your own
hardware, education and research. It does not transmit during scanning, capture
payloads, or decrypt traffic. The two active features (open-network header
decoding and BLE Device-Information reads) are **off by default** and opt-in.

You are responsible for complying with the laws of your jurisdiction regarding
radio monitoring and packet capture. Capturing the *contents* of communications
can be illegal even on unencrypted networks in some places. This project
deliberately limits itself to headers, identifiers, and devices' own public
announcements.

## License

**GPL-3.0-or-later**, matching upstream SignalSweep. See `LICENSE`.

Bundled third-party code:
- Detection signatures & Remote ID integration — from
  [SignalSweep](https://github.com/B0z0dcl0wn/SignalSweep) (GPL-3.0-or-later)
- `opendroneid.c/.h`, `odid_wifi.*` — the
  [OpenDroneID](https://github.com/opendroneid/opendroneid-core-c) core library
  (Apache-2.0)
- Flock OUI research compiled by OrdoOuroborous / @NitekryDPaul
- `User_Setup.h` for the CYD — from Rui Santos / Random Nerd Tutorials

## Acknowledgements

- **[B0z0dcl0wn](https://github.com/B0z0dcl0wn/SignalSweep)** — the original
  SignalSweep, which this is built on.
- **[skizzophrenic/SquachWatch-CYD](https://github.com/skizzophrenic/SquachWatch-CYD)**
  — a related gamified detector for the same board.
- **[witnessmenow](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)**
  and the CYD community for documenting this board.
