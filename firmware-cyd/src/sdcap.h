// SPDX-License-Identifier: GPL-3.0-or-later
// sdcap.h — capture to the CYD's microSD as Wireshark-readable pcapng (issue #10). PASSIVE.
//
// The C5 streams the focused target's frames (SS_EVT_CAPTURE, see firmware-c5/main/capture.h); this module
// writes them to /signalsweep/cap_NNN.pcapng (radiotap + 802.11) and, when the capture ends, a sidecar
// cap_NNN.txt with the Wireshark display filter and a name map. Data frames arrive already cut to their
// 802.11 header, so no payload is ever written. Nothing here transmits.
//
// The microSD slot has VSPI (GPIO 18/19/23, CS 5) to itself: the touch controller is bit-banged on its own pins.
#pragma once
#include <Arduino.h>

enum SdCapState : uint8_t { SDC_IDLE = 0, SDC_REC = 1, SDC_STOPPING = 2 };

struct SdCapStats {
    char     file[40] = {};          // e.g. /signalsweep/cap_003.pcapng
    uint32_t frames = 0;             // written
    uint32_t bytes = 0;              // pcapng bytes written (including staged)
    uint32_t dropped = 0;            // frames the C5 had to drop (its queue was full)
    uint32_t start_ms = 0;
};

bool        sdcap_card_ok(void);                 // mounts on first use; false if there is no usable card
const char* sdcap_card_text(void);               // "ready 3.7 GB", "no card", ...
bool        sdcap_start(const char* mac, const char* label, uint8_t ch, bool ble);   // open the file, begin recording
void        sdcap_stop(const char* why);         // ask for the end: the C5 drains, then the file closes
void        sdcap_abort(const char* why);        // close now (link reset)
void        sdcap_event(const uint8_t* cbor, size_t n);   // an SS_EVT_CAPTURE payload (after the kind byte)
void        sdcap_tick(uint32_t now_ms);         // flush every couple of seconds; time out a stalled stop
SdCapState  sdcap_state(void);
bool        sdcap_wanted(void);                  // true while recording: this is what SET_CONFIG {cap} carries
const SdCapStats& sdcap_stats(void);
const char* sdcap_last_msg(void);                // why the last capture ended, or an error
