# Credits

## Original project
**SignalSweep** by **B0z0dcl0wn**
https://github.com/B0z0dcl0wn/SignalSweep (GPL-3.0-or-later)

This repository is a port and expansion of SignalSweep for the ESP32 "Cheap
Yellow Display". The detection engine — signature table, scoring, tracker and
drone logic — originates there and is used under GPL-3.0. Please support the
original author first.

## Research & data
- **Flock OUI set** — compiled by OrdoOuroborous / @NitekryDPaul
  ("GoFlockYourself" extended OUIs), via SignalSweep.
- **DJI DroneID Wi-Fi IE layout** — Kismet project
  (dot11_ie_221_dji_droneid), and "DJI drone IDs are not encrypted" (2022).

## Bundled libraries
- **OpenDroneID core** (opendroneid.c/.h, odid_wifi.*) — Apache-2.0,
  https://github.com/opendroneid/opendroneid-core-c
- **CYD User_Setup.h** — Rui Santos / Random Nerd Tutorials.

- **MAC vendor names** - IEEE Registration Authority registries (MA-L, MA-M, MA-S), https://standards.ieee.org/products-programs/regauth/
- **Bluetooth company, service and member names** - Bluetooth SIG Assigned Numbers (public repository).
  Both are fetched by `tools/gen_vendor_tables.py` into `firmware-cyd/include/vendor_db_data.h`.

## Hardware community
- **witnessmenow / ESP32-Cheap-Yellow-Display** — board docs and the "CYD" name.
- **skizzophrenic / SquachWatch-CYD** — related detector for the same board.

## This port
- Touch UI, focus mode, device profiling, vendor DB, and additional
  2.4 GHz fingerprints: pbarsamian, with Claude (Anthropic) assisting.
