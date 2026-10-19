# SignalSweep CYD — Session Handoff (split architecture M2; standalone v3.21 on `standalone` branch)

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
  - HUNT: v3.20; LINKS/TRAFF: v3.21; CAP: placeholder ("coming in a later version")
- Serial: [focus] lines show locked channel + target frame/advert counts

## Hunt mode (v3.20)
- HUNT sub-tab: locks onto the focused device, click + screen pulse + RGB LED
  all speed up as RSSI rises (geiger style); silent after 8 s no contact
- Big live RSSI, WARMER/colder trend, peak, seconds-since-heard
- hunt_interval(): -95 dBm ~1.2 s -> -35 dBm ~70 ms
- FIX: speaker LEDC channel 1 was never initialized (alert beeps were silent) -
  now ledcSetup(1,2000,8)+ledcAttachPin(PIN_SPEAKER,1) in ui_init
- Leaving HUNT / Back / main-tab all call hunt_all_off()

## Links + traffic (v3.21a)
- While a WiFi target is focused the promiscuous filter widens to MGMT+DATA
  (scanner_wifi_data_filter, toggled by the scan task on focus enter/leave)
- focus_note_peer(): headers only. Tallies per-peer frames each way, bytes, QoS,
  RSSI (peer's own tx), role from ToDS/FromDS (+assoc frames): STA vs AP.
  16-slot table in FocusProfile, stalest evicted. No payloads read.
- TRAFF: peers by frame count (>to <from bytes Q=QoS rssi). LINKS: OBSERVED
  (AP/clients, probed SSID, same MAC on BLE+WiFi), INFERRED (sibling MACs: octets
  0-4 equal ignoring LA bit, last octet within 16; devices probing the target SSID;
  registry only), CORRELATED (placeholder: needs SD history, v3.21b)
- Peer rows are tappable: pivot_to() focuses that device; unknown WiFi peers are
  added to the registry as UNKNOWN "Peer (pivot)" (hidden from the LIST tab)
- Scan task refreshes the focused entry's timestamp/RSSI whenever the target is
  heard transmitting, so pivoted peers don't age out and HUNT tracks them
- BLE focus: LINKS/TRAFF state plainly that BLE connections are not observable
- UNTESTED on hardware as of this commit (builds clean, flash 33%)

## Split architecture status (main)
- M1 (shared contract: ss_capabilities.c, ss_frame, ss_cbor, ss_token) and M2 (UART link:
  CYD uart_link + C5 uart_slave/dispatch, canned scans) are written. Both firmwares
  compile (CYD flash 10.9%; C5 IDF 5.5.6 esp32c5, 257 KB). Not run on hardware yet:
  see BRINGUP.md for the flash steps and the acceptance-check table.
- ESP-IDF v5.5.6 is installed on the dev machine at ~/esp/esp-idf (tools in
  ~/.espressif); activate with `. $HOME\esp\esp-idf\export.ps1`.
- CYD dev console (serial) can probe the C5 but cannot change mode or mint tokens
  (those need the M4 pop-up and M6 consent flow).
- Next: M3 real passive radio on the C5 (scan_sched, signatures/remote_id port).

## Split architecture hardware (planned; see BUILD_MANIFEST.md / PROTOCOL.md)
- UART link LOCKED: CYD CN1 GPIO22=TX, GPIO27=RX (Serial2) <-> XIAO C5 GPIO9=TX (D9),
  GPIO10=RX (D10); GND common; 3.3 V red wire unpopulated. Crossover required.
  Still to verify physically: CN1 pin-1 end / GPIO22 populated, pigtail colors.
- Power: one LiPo -> switch -> C5 BAT pads + boost input (spliced at the battery);
  boost 5 V -> CYD only, NEVER the C5 5V pad (charger input shares that net).
  Charging = USB-C into the C5 (SGM40567, ~200 mA fixed per schematic Rev V1.1).
  Use a protected cell; no protection IC found on the C5 board.
- Battery gauge: C5 reads VBAT/2 (GPIO26 enables divider, GPIO6/ADC1_CH5 reads) ->
  `batt_mv` in GET_STATUS / STATUS event. Charge state not readable (LED only).
- None of the above has been wired or measured yet.

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
