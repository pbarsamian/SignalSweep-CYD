# Parts list

Two builds, two lists. Quantities are one of each. No prices or part numbers are given: they move,
and several parts below have more than one acceptable version. Items marked **verify** are what the
project's own notes ask you to confirm against your actual board before relying on them.

## Option A: CYD only (standalone)

| Part | Notes |
|---|---|
| CYD, **ESP32-2432S028R**, single micro-B USB, ILI9341 display | The 2-USB (USB-C) variant needs a different `User_Setup.h`; clones vary in display driver and colour inversion. See the README's CYD section. |
| USB cable, micro-B, **data-capable** | Charge-only cables will not flash. |

That is the whole list. The CYD's 2.4 GHz-only radio is the limit of this build.

## Option B: the full kit

### Required for bench bring-up

| Part | Notes |
|---|---|
| CYD, ESP32-2432S028R (as above) | The control plane and the touchscreen. |
| **Seeed Studio XIAO ESP32-C5** | The radio: dual-band Wi-Fi 6 and BLE 5. It is ESP-IDF only here. Its schematic (Rev V1.1) is the source for the charger and battery notes; **verify** the revision marking on your board. |
| USB-C cable, data-capable | C5: flashing, logs, and the battery charging input. |
| USB cable, micro-B, data-capable | CYD: flashing and logs. |
| Three wires | CYD GPIO27 to C5 D9, CYD GPIO22 to C5 D10, and ground. Both are 3.3 V logic: no level shifter. **Leave the 3.3 V wire unconnected.** |

### Required for a self-contained, battery-powered unit

| Part | Notes |
|---|---|
| **Protected** single-cell LiPo | The C5 board has no battery protection circuit that we found. About **400 mAh or more** keeps the 200 mA charge current at or below 0.5C. Check polarity against the C5's BAT pads: a reversed cell can destroy the board. |
| Switch (SPST) | In the battery positive line, ahead of both the C5 and the boost converter. |
| 5 V boost converter | Input range must cover about 3.0 to 4.2 V. It runs the CYD, and draws roughly 0.5 A at 3.7 V in, so size it and its wiring for that. A **low-voltage cutoff** is wanted, since the C5 board has none. An **EN pin** is useful for the optional charge-mode supply cut (issue #5). Its 5 V output goes to the CYD only, never to the C5's 5V pad. |
| Wire for the battery and boost runs | Heavier than the signal wires; the boost input is spliced at the battery and switch, not through the C5's BAT pads. |

### Antenna and optional parts

| Part | Notes |
|---|---|
| Dual-band 2.4 / 5 GHz antenna | Needed for 5 GHz detection to be worth doing. The notes plan a U.FL to SMA pigtail with a dual-band antenna; **verify** the antenna connector on your XIAO ESP32-C5. |
| About 470 µF capacitor | Near the C5's supply pins, as a precaution for radio bursts. Not verified as required. |
| 4-wire pigtail for the CYD's CN1 socket | A convenient way to reach GPIO22, GPIO27 and ground. The CYD has look-alike sockets; CN1 is GND, IO22, IO27 and 3.3 V. Confirm which is which with a meter before powering. |
| microSD card | Not used yet. It is for the planned capture and export features (issues #10 and #15). The SD slot shares a bus with the touch controller. |
| USB power meter and a multimeter with a mA range | Only for measuring charge-mode current (issue #5). |

### Not part of the kit

- **Speaker:** the CYD has an amplifier for one, but some boards ship without the speaker fitted, and
  alert beeps are then silent. Everything else works without it.
- **Meshtastic node:** a Heltec node is only needed for the planned alert relay (issue #12).

## Where the wiring is documented

The pin map, the battery and boost wiring, and the safety notes are in
[`BUILD_MANIFEST.md`](../BUILD_MANIFEST.md) section 2, with the checks to run before powering in
[`BRINGUP.md`](../BRINGUP.md). A 3D-printable case is planned: see [`hardware/case/`](../hardware/case/README.md).
