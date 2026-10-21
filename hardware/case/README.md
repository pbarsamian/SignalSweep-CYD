# 3D-printable case: placeholder

Case files are not designed yet. This folder is where they will go.

## What will live here

- the printable models (STL for printing, plus the editable source, STEP or 3MF)
- print notes: orientation, material, supports, layer height
- photos of the assembled unit

## What the case has to handle

These come from the build, not from a finished design, so treat them as requirements to be checked
against real dimensions:

- **Two boards, one case:** the 2.8 inch CYD with its touchscreen facing out, and the XIAO ESP32-C5.
- **Both USB ports reachable:** the CYD's micro-B port (flashing and logs) and the C5's USB-C port
  (flashing, logs, and the only battery charging input).
- **A power switch** that can be reached with the case closed.
- **An antenna path:** the C5 needs a dual-band antenna for 5 GHz, so the case needs a hole or an
  SMA bulkhead for it.
- **A protected LiPo and a 5 V boost converter** inside, with room for the wiring and a cable
  route from the battery and switch to both boards.
- **The CN1 cable** between the boards, and clearance for the CYD's RGB LED and light sensor.
- **Heat and strain relief:** the boost converter carries about 0.5 A, so it should not be
  boxed in against the battery.

## Open questions

- Final board and battery dimensions (measure your own; CYD clones vary).
- Whether the antenna sits on the top edge or the back.
- Wall-mount, desk-stand or handheld orientation.

If you design one, a pull request with the files and a photo is welcome.
