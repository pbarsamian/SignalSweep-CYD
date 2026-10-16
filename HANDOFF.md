# SignalSweep CYD — Session Handoff (v3.19)

## Board: ESP32-2432S028R "Cheap Yellow Display"
- ILI9341 (HSPI), rotation 3. **Panel colour fix (confirmed by boot test MODE 2):**
  after setRotation(3): writecommand(0x36); writedata(0xE8); invertDisplay(true)
- XPT2046 touch (VSPI). **Both axes inverted** (confirmed): 
  ty = map(rx, X_MAX, X_MIN, 0, 239); tx = map(ry, Y_MAX, Y_MIN, 0, 319); clamp
- Calibration X 286-3823, Y 174-3784. RGB LED active LOW 4/16/17, speaker 26, BL 21

## Scope (confirmed good)
- WiFi: beacons only, tapered hump at ((ch-1)*319)/10, decay 4 (v3.2 logic)
- BLE: boost peaks at buffer 0/191/319 (v3.8 logic)
- Drawing: bright top half / dim bottom, si = (x*319)/(sw-1)

## Detection (v3.17): ported from upstream B0z0dcl0wn/SignalSweep
- src/signatures.cpp: all 96 upstream default rules (mechanically diffed = identical)
- Upstream weights; list at sum >= 60, alert at strongest single signal >= 70
- One alert per appearance; entries go stale after 2 min (upstream)
- BLE: rules (AND semantics, exact UUID match), AirTag long-form 0x12/0x19 only,
  Google FMDN 0xFEAA frame 0x40/0x41, Tile 0xFEED, Samsung 0xFD5A/0xFD59,
  Flock "Penguin" battery, Remote ID 0xFFFA (vendored opendroneid, Apache-2.0)
- BLE scan: active, duplicates on, 100/50, setMaxResults(0) (upstream)
- WiFi: OUI rules, Flock wildcard probe (+Lite-On IE), "Flock-xxxxxx" SSID,
  SSID-prefix rules, Remote ID beacon IE + NAN
- Channel box: "WiFi Ch N" for WiFi detections, "BLE" for BLE

## CYD additions beyond upstream (v3.17, sourced)
- DJI DroneID vendor IE 26:37:12 (subcmd 0x10/0x11, Kismet layout) -> Drone, 90
- SSIDs TELLO-, ANAFI-, Anafi_ -> Drone, 80
- DJI + Parrot OUIs -> Drone, 30 (corroboration only)
- Hak5 00:13:37 -> Hacking gear, 60 (listed, no beep)

## Info view (v3.18)
- Scrollable, tracked by MAC (not list index); live refresh 1 s in place;
  shows last data + "aged out" if the device is pruned
- Identity: type, confidence (+corroboration), MAC, address type, IEEE vendor
  (only for public/universal MACs), BT SIG company, device name, SSID / hidden /
  probing-for, Apple Continuity role, BLE appearance
- Signal/timing: RSSI now (min..max), TX power + rough distance, channel + MHz,
  heard via BLE/WiFi, first seen, last seen, times heard
- BLE: connectable, up to 4 services with SIG names, custom 128-bit flag
- WiFi: frame type, security (RSN AKM parse), 802.11 generation, country, beacon int
- Remote ID: UAS ID, UA type, operator ID, position, height, altitude, speeds,
  heading, pilot position

## Focus mode + sub-pages (v3.19)
- Tapping a detection STOPS sweeping and works that one target (focus.h/.cpp)
  - WiFi: radio locks to target channel; BLE: 100% scan window
  - Back button or leaving DETAIL via tab bar resumes sweeping
- Detail sub-tabs: INFO | LINKS | TRAFF | RAW | HUNT | CAP
  - INFO: scrollable rows + focus identity (WPS maker/model/serial/name,
    spatial streams, vendor-IE chipset hints, live frame/advert counts)
  - RAW: BLE advertising hex, or 802.11 elements-present decode + vendor IE count
  - LINKS/TRAFF/HUNT/CAP: placeholders ("coming in a later version")
- Serial: [focus] lines show locked channel + target frame/advert counts

## Vendor database
- platformio.ini uses huge_app.csv (3 MB app, no OTA) — CHECK flash % in build output
- Shipped include/vendor_db_data.h is a small fallback (VENDOR_DB_FULL 0)
- Run once with internet:  python tools/gen_vendor_tables.py
  (IEEE MA-L/MA-M/MA-S CSVs + Bluetooth SIG company/UUID YAML; --local DIR offline)

## Not yet ported from upstream
- Attack detectors: BLE popup spam, deauth/disassoc bursts, Karma/PineAP, pwnagotchi
- Tag-flood collapse (spoofed tracker storms)
- "Listed, never beeps" vendors (Motorola, Verkada, Axis...) map to UNKNOWN: matched, not shown
- BLE+WiFi corroboration bonus (+15)

## Build
python -m platformio run --target clean
python -m platformio run --target upload
python -m platformio device monitor --baud 115200
