# SignalSweep CYD

A passive, receive-only counter-surveillance scanner you can hold in your hand.
It listens for Bluetooth LE and Wi-Fi (2.4 GHz and 5 GHz) emissions, flags devices
that match known surveillance hardware (ALPR cameras, body cameras, personal
trackers, drones broadcasting Remote ID, Wi-Fi attack gear), and lets you tap any
detection to investigate it: lock onto it, walk toward it with a geiger-style
signal meter, see who it talks to, and read what it announces about itself.

It never transmits while scanning and never tries to decrypt anything.

> **Status:** active development. The two-board build (the "full kit") is the
> main line and is verified on hardware. Several features are still on the
> backlog; see [What is built and what is not](#what-is-built-and-what-is-not).

## What this is now, and what it started as

**It started** as a port of the [SignalSweep](https://github.com/B0z0dcl0wn/SignalSweep)
detector onto the ~$15 ESP32 "Cheap Yellow Display" (CYD): one board doing everything,
Wi-Fi and BLE time-sliced because that chip cannot run both radios at once, 2.4 GHz only.
That version still exists as the [standalone build](#option-a-cyd-only-standalone).

**It is now** a small system of its own:

- **A split architecture.** A **Seeed XIAO ESP32-C5** is the radio (dual-band Wi-Fi 6
  plus BLE 5, ESP-IDF). The **CYD** is the control plane and the screen: it owns the
  one authoritative RF mode, draws the UI, and tells the C5 what to do. They talk over
  a 3-wire UART with a framed, CRC-checked, CBOR wire protocol
  ([`PROTOCOL.md`](PROTOCOL.md)). A laptop interface over MCP is designed for but not
  built yet.
- **5 GHz coverage**, and a **scan scheduler** that shares the one C5 radio between
  Wi-Fi and BLE with selectable balance presets.
- **A touch UI**: live scope, a filterable and sortable detection list, a per-device
  detail view, and settings.
- **Investigation tools**: focus mode (park the radio on one target), hunt mode,
  a links and traffic view of who a device talks to, raw advertisement and element
  inspection, vendor and company names from the full IEEE and Bluetooth SIG
  databases, and an opt-in open-network profiling mode (headers and announcements only).
- **A safety model**: passive by default, and the transmit side of the framework is
  deliberately left empty. See [Legal and ethical use](#legal-and-ethical-use).

## Choose your build

| | **A. CYD only (standalone)** | **B. Full kit (CYD + XIAO ESP32-C5)** |
|---|---|---|
| Boards | 1: the CYD | 2: the CYD and a XIAO ESP32-C5, wired together |
| Wi-Fi | 2.4 GHz only | 2.4 GHz and 5 GHz |
| Wi-Fi and BLE | time-sliced, 6 s each | scheduler: concurrent or sliced, adjustable balance |
| Where it lives | `standalone` branch / tag `standalone-v3.21` | `main` (this branch) |
| Status | frozen at v3.21a, works on its own | active development |
| Needs | the CYD and a USB cable | the parts in [`docs/PARTS.md`](docs/PARTS.md) |

If you only want to try it, start with **A**: it needs nothing but the board.

## Option A: CYD only (standalone)

The original single-board firmware. It lives on the `standalone` branch, and the
same commit is tagged `standalone-v3.21`.

**What you need:** a CYD (ESP32-2432S028R, single micro-B USB, ILI9341; see
[Hardware: the CYD](#hardware-the-cyd)), a data-capable USB cable, and
[PlatformIO](https://platformio.org/) (`pip install platformio`, or the VS Code extension).

```bash
# Get the standalone firmware without disturbing the main checkout
git clone https://github.com/pbarsamian/SignalSweep-CYD.git
cd SignalSweep-CYD
git worktree add ../SignalSweep-standalone standalone-v3.21
cd ../SignalSweep-standalone

# Optional, needs internet once: the full IEEE + Bluetooth SIG vendor database.
# Without it a small built-in list is used and vendor names are less complete.
python tools/gen_vendor_tables.py

python -m platformio run --target clean
python -m platformio run --target upload          # builds and flashes the CYD
python -m platformio device monitor --baud 115200
```

(`git checkout standalone-v3.21` in a normal clone works too, if you do not need `main`.)
This builds cleanly: flash use is about 33% of the 3 MB app partition. The first
build downloads the libraries and takes a minute or two.

`User_Setup.h` (the display configuration) is copied into the TFT_eSPI library
automatically by `copy_setup_pre.py` on every build. Nothing to move by hand.

A phone hotspot named `TELLO-TEST01` (2.4 GHz) is a harmless positive test: it fires the
DJI Tello rule within one Wi-Fi slot (about 12 s).

## Option B: the full kit (CYD + XIAO ESP32-C5)

### 1. Parts

See [`docs/PARTS.md`](docs/PARTS.md) for the full list, including battery, boost converter
and antenna. For a first bench bring-up you only need the two boards, two USB cables and
three jumper wires, and each board is powered from its own USB.

### 2. Wire the UART link

Only three wires connect the boards. **Do not connect the 3.3 V wire.** The two data
wires cross over: each board's TX goes to the other's RX.

| CYD CN1 | | XIAO ESP32-C5 pad | Note |
|---|---|---|---|
| GPIO27 (CYD RX) | ← | D9 / GPIO9 (C5 TX) | crossover |
| GPIO22 (CYD TX) | → | D10 / GPIO10 (C5 RX) | crossover |
| GND | | GND | required |

Use the **CN1** socket on the CYD (GND, IO22, IO27, 3.3 V). The CYD has other look-alike
4-pin sockets; the wrong one gives a one-way link. Confirm CN1 with a meter before
powering: one end pin is a steady 3.3 V, the opposite end is GND. The full pin notes and
the mistakes already made are in [`BUILD_MANIFEST.md`](BUILD_MANIFEST.md) section 2 and
[`BRINGUP.md`](BRINGUP.md).

### 3. Flash the C5 (ESP-IDF)

You need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/get-started/)
**5.5 or newer** (developed on 5.5.6) installed and exported in your shell.

```powershell
# Windows PowerShell, once per shell
. $HOME\esp\esp-idf\export.ps1
```
```bash
# Linux / macOS
. $HOME/esp/esp-idf/export.sh
```

Then, from the repo root (the first time only, run `set-target`; it creates `sdkconfig`
from `sdkconfig.defaults`):

```bash
cd firmware-c5
idf.py set-target esp32c5
idf.py build
idf.py -p COM10 flash monitor     # use your port: COMx on Windows, /dev/ttyACM0 on Linux
```

Quit the monitor with `Ctrl+]`. If the port does not appear, hold the C5's **BOOT**
button while plugging in the USB-C cable. The C5's USB-C port is both its flashing and
log port, and it is also the only charging input.

**Files and addresses**, if you would rather flash with `esptool` yourself. After a build,
these are in `firmware-c5/build/` (the repo does not publish prebuilt binaries yet):

| Address | File |
|---|---|
| `0x2000` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0x10000` | `signalsweep_c5.bin` |

```bash
cd firmware-c5/build
esptool.py --chip esp32c5 -p COM10 -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 signalsweep_c5.bin
```

Use the `esptool` that ships with ESP-IDF 5.5 (older copies do not know the ESP32-C5).
The flash layout is a 3 MB app with no OTA ([`firmware-c5/partitions.csv`](firmware-c5/partitions.csv)).

### 4. Flash the CYD (PlatformIO)

```bash
cd firmware-cyd
python -m platformio run --target upload --upload-port COM11    # your CYD's port
python -m platformio device monitor --port COM11 --baud 115200
```

A tip from the development machine (Windows): `idf.py` was run from PowerShell and PlatformIO from
a separate Git Bash shell. Mixing them in one shell picked the wrong Python for one of the tools.

Everything the CYD build needs is handled for you by two pre-build hooks:

- `copy_setup_pre.py` copies `firmware-cyd/config/User_Setup.h` into the TFT_eSPI library
  folder (`.pio/libdeps/*/TFT_eSPI/User_Setup.h`) on every build. PlatformIO overwrites that
  file when it refreshes the library cache, which is why this is a hook and not a one-time copy.
  If you ever need to do it by hand, copy it to exactly that path and rebuild with
  `python -m platformio run --target clean` first.
- `about_pre.py` stamps the build with the git revision (shown on the About page) and
  generates the credits text from `CREDITS.md`.

The vendor database is already committed (`firmware-cyd/include/vendor_db_data.h`, about
1 MB of flash). To refresh it from the IEEE and Bluetooth SIG sources, run
`python tools/gen_vendor_tables.py` (internet needed once).

**Files and addresses**, for `esptool`. After a build these are in `firmware-cyd/.pio/build/cyd/`:

| Address | File |
|---|---|
| `0x1000` | `bootloader.bin` |
| `0x8000` | `partitions.bin` |
| `0xe000` | `boot_app0.bin`, from `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/` |
| `0x10000` | `firmware.bin` |

```bash
# pip install esptool   (PlatformIO also bundles one)
python -m esptool --chip esp32 -p COM11 -b 921600 write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

The CYD uses PlatformIO's `huge_app` partition scheme (3 MB app, no OTA) to fit the vendor database.

### 5. Check that it is working

Within about 2 s of both boards running:

- The link indicator in the CYD header comes up, and **SET > ABOUT** shows both firmware versions (the C5 line reads "not linked" until the handshake succeeds).
- Tap **SCOPE** and you should see access points on 2.4 GHz and 5 GHz and, with the scan
  preset on BALANCED, the scope cycling 2.4 GHz, 5 GHz and BLE.
- On the CYD serial console, press `w`: `[aps] N access points` with `2G` and `5G` rows.

The step-by-step bring-up checks (every console key with its expected result) are in
[`BRINGUP.md`](BRINGUP.md).

### 6. Power, battery and the case

The battery, switch and boost-converter wiring, the charging notes and the safety warnings are in
[`BUILD_MANIFEST.md`](BUILD_MANIFEST.md) section 2 ("Power"). Read it before connecting a cell: the
C5 board has no battery protection circuit, so **use a protected cell**.

3D-printable case files are a placeholder for now: see [`hardware/case/`](hardware/case/README.md).

## Using it

| Tab | What it does |
|---|---|
| **SCOPE** | Live view of what the radio is scanning: 2.4 GHz, 5 GHz or the BLE list. AUTO follows the radio; tap the band button to hold one. Wi-Fi bands list the strongest APs; tap one to open and focus it. |
| **LIST** | Everything detected. Filter by type, radio or alerts only, sort by strength, newest or confidence, and jump around with the scroll strip. |
| **INFO** | One device in depth: identity, vendor, signal, timing, decoded fields, and sub-pages LINKS, TRAFF, RAW, HUNT. Tapping a detection starts **focus mode**. |
| **SET** | ALERTS (detection toggles, speaker, LED, scan balance), DEVICE (RF mode, charge mode, brightness, diagnostics), PROFILE (open-network profiling switch) and ABOUT (project URL, credits, firmware versions). |

## Serial debuggers

There are two serial ports, one per board, and each needs its own terminal. The port
numbers change when a cable is reseated, so check them again after any USB drop
(Windows: Device Manager, or `python -m serial.tools.list_ports`).

| Board | Port | How to open it |
|---|---|---|
| **CYD** | CH340 USB-serial (micro-B), 115200 baud | `python -m platformio device monitor --port COMx --baud 115200` |
| **C5** | native USB Serial/JTAG (USB-C) | `idf.py -p COMx monitor` (quit with `Ctrl+]`) |

The C5's link to the CYD uses separate UART pins (GPIO9/10), so its USB port is only for
flashing and logs. The C5 console is log-only; the CYD console accepts single-key commands.

**Opening a port from your own script can reset a board**, depending on the USB-serial
adapter's DTR and RTS lines. The development scripts open the port with DTR and RTS both
set to false.

### CYD console keys

Type one character in the CYD monitor. `?` prints the help.

| Key | What it does |
|---|---|
| `h` | HELLO again (new session; focus and open-network profiling reset) |
| `s` | GET_STATUS |
| `w` | SCAN_WIFI: prints the access point table (`[aps]`) |
| `b` | SCAN_BLE: prints BLE devices |
| `l` | LIST_DETECTIONS: prints the C5's detection list |
| `p` | profile of the first listed detection |
| `c` / `e` | enter / leave charge mode |
| `t` | simulate a screen tap (wakes the screen in charge mode) |
| `f` | focus the first listed detection |
| `F` | focus the **strongest access point in range**, which may not be yours |
| `k` | print the focus view (counters, peers, and open-network results when on) |
| `r` | release focus |
| `o` | toggle open-network profiling (same as SET > PROFILE) |
| `1` `2` `3` `4` | scan balance presets: WI-FI, BALANCED, BLE, SLICED (not saved) |
| `g` / `a` | gated operations without a token: expect `ERR_MODE` |
| `u` / `x` / `v` | unknown opcode, bad CRC, wrong protocol version: expect `ERR_OPCODE`, `NACK ERR_CRC`, `NACK ERR_VERSION` |

The console cannot change the RF mode or mint tokens.

### What the logs look like

CYD prefixes: `[link]` frame counters every 2 s (quickest wiring triage: `raw=0` means nothing
is arriving on GPIO27; `crc` rising means corruption; `rx` rising means healthy), `[aps]`, `[list]`,
`[profile]`, `[focus]`, `[onet]`, `[scan]` (radio, focus lock and Wi-Fi band changes), `[sched]`,
`[power]`, `[det]` (new detections and alerts).

C5 lines (every 5 s unless noted): `ble: scan=ON adv=... devs=... hits=...`,
`wifi: radio=ON sweeps=... mgmt=... APs 2.4G=... 5G=...`, `det: NEW BLE '...' ... ALERT`
when a detection is first seen, and `HELLO ok, session started` on every handshake.

## What is built and what is not

**Built and verified on hardware (full kit):** passive 2.4 GHz and 5 GHz Wi-Fi scan, passive BLE scan,
the detection engine and signature set, the scan scheduler and band dwell, the touch UI (scope,
filterable list, info, settings, about), focus mode (channel lock, full-duty BLE), hunt mode,
LINKS, TRAFF and RAW views, the vendor database, ASTM F3411 Remote ID decode, charge mode,
and opt-in open-network profiling (DHCP hostname and class and port tallies verified; mDNS and
SSDP not yet exercised on real traffic).

**Not built yet** (tracked as [issues](https://github.com/pbarsamian/SignalSweep-CYD/issues)):
the consent flow and token-gated BLE interrogation (#4), charge-current measurement (#5), the
laptop MCP server (#9), pcapng capture to microSD (#10), a Meshtastic alert relay (#12), and
list export (#15).

## Repository layout

```
shared/          the frozen contract compiled into both firmwares (protocol, CBOR, capability registry, tokens)
firmware-cyd/    the CYD: control plane and touch UI (PlatformIO, Arduino)
firmware-c5/     the XIAO ESP32-C5: the radio (ESP-IDF 5.5+, target esp32c5)
tools/           vendor database generator
docs/            parts list
hardware/case/   3D-print case files (placeholder)
```

| Document | What is in it |
|---|---|
| [`BUILD_MANIFEST.md`](BUILD_MANIFEST.md) | the architecture, hardware facts, power plan, and the scope boundary |
| [`PROTOCOL.md`](PROTOCOL.md) | the CYD-to-C5 wire protocol |
| [`BRINGUP.md`](BRINGUP.md) | wiring and acceptance checks for a new build |
| [`HANDOFF.md`](HANDOFF.md) | technical state: what is verified, and board quirks |
| [`CREDITS.md`](CREDITS.md) | everyone and everything this builds on |
| [`CLAUDE.md`](CLAUDE.md) | context file for the AI coding assistant used on this project |

## Hardware: the CYD

The CYD is the **ESP32-2432S028R**, nicknamed the "Cheap Yellow Display" by the community around
it. If you are new to the board:

- **[witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)**:
  the community hub for pin-outs, setup and troubleshooting.
- **[Getting started guide](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/Instructions/SetupAndConfiguration.md)**:
  first-time setup and the CH340 USB driver.
- **[2-USB (USB-C) variant display config](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/DisplayConfig/CYD2USB/User_Setup.h)**:
  if your board has two USB ports you may need this `User_Setup.h` instead.

This project targets the **single-USB micro-B variant with the ILI9341 display**. The confirmed-working
display config is `firmware-cyd/config/User_Setup.h`.

> **CYD boards and clones vary**, especially the display driver (ILI9341 or ST7789) and colour
> inversion. If colours look wrong (red shows as blue), see the colour-order note in
> [`HANDOFF.md`](HANDOFF.md): this board needed MADCTL `0xE8` and `invertDisplay(true)`.
> Some boards also ship without the speaker fitted, in which case alert beeps are silent.

## What this can and cannot see (drones)

- **Detectable:** standards Remote ID (Wi-Fi beacon / NAN, BLE); the DJI DroneID vendor IE
  26:37:12 on Wi-Fi; the DJI BLE presence beacon (OUI 48:1C:B9 + service UUID 0xFFF0, ground
  detection, pending a hardware test); Tello and ANAFI SSIDs; DJI OUIs and `DJI-`/`MAVIC`
  SSIDs as weak corroboration (listed, never alert alone).
- **Not detectable on an ESP32:** OcuSync DroneID (an OFDM burst on 2.4/5.8 GHz). Decoders such
  as proto17/dji_droneid need an SDR and GNU Radio.
- **Likely not receivable:** "Enhanced Wi-Fi" drones (Spark/Air/Mini era) reportedly use a
  5 MHz quarter-rate Atheros ad-hoc mode. Unverified for ESP32; confirm before relying on it.
- Recent models (Mini 4 Pro, OcuSync 4) may only be catchable on the ground via BLE.

## Legal and ethical use

This is a **receive-only** tool for understanding the radio environment around you: spotting
surveillance devices, auditing your own hardware, education and research. It does not transmit
while scanning, does not decrypt anything, and does not store the contents of other people's
traffic.

The one mode that reads beyond frame headers, **open-network profiling**, is off by default, never
runs on encrypted frames, never reads DNS, TCP payloads or queries, and is limited to a device's
own announcements (DHCP hostname, mDNS and SSDP) and port tallies. Even so, use it only on networks
you own or are authorised to test.

The framework has a consent and token design for future active operations, but **no transmit
handlers exist** (no deauthentication, spoofing, jamming or similar), and the code leaves those
slots empty on purpose (`BUILD_MANIFEST.md` section 0).

You are responsible for complying with the laws of your jurisdiction on radio monitoring and packet
capture. Capturing the contents of communications can be illegal even on unencrypted networks in some places.

## License

**GPL-3.0-or-later.** See [`LICENSE`](LICENSE). Bundled third-party code and data:

- the detection signature set and parts of the detection logic carried over from
  [SignalSweep](https://github.com/B0z0dcl0wn/SignalSweep) (GPL-3.0-or-later)
- `opendroneid.c/.h`, `odid_wifi.*`: the [OpenDroneID](https://github.com/opendroneid/opendroneid-core-c) core library (Apache-2.0)
- Flock OUI research compiled by OrdoOuroborous / NitekryDPaul
- `User_Setup.h` for the CYD: from Rui Santos / Random Nerd Tutorials
- IEEE Registration Authority MAC registries and Bluetooth SIG Assigned Numbers, used to generate the vendor database

## Credits and inspiration

This project began as a port of **[SignalSweep](https://github.com/B0z0dcl0wn/SignalSweep) by
B0z0dcl0wn**, and it took its starting point from there: the idea of a passive scanner, the
detection philosophy (a rule that fires on ordinary consumer hardware is worse than no rule),
and the original signature set. Thank you. If you like this, go star the original.

It has since grown in a different direction (two boards, a new protocol, 5 GHz, a new UI, and the
investigation tools), and its signature set and some of its detection logic are still carried over
under GPL-3.0-or-later, with attribution. Everything is listed in [`CREDITS.md`](CREDITS.md).

Also with thanks to **[skizzophrenic/SquachWatch-CYD](https://github.com/skizzophrenic/SquachWatch-CYD)**
(a related detector for the same board), **[witnessmenow](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)**
and the CYD community for documenting the board, and the OpenDroneID and Kismet projects.
