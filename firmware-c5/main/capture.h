// SPDX-License-Identifier: GPL-3.0-or-later
// capture.h — targeted frame capture for the CYD's microSD (issue #10). PASSIVE.
//
// While a Wi-Fi focus is active and the CYD has switched capture on, every frame to or from the focused MAC
// is queued here and streamed to the CYD (SS_EVT_CAPTURE), which writes it to a pcapng file. Data frames are
// cut to their 802.11 header: no LLC, no IP, no payload, so no handshakes (EAPOL is a data frame) and nothing
// of anyone's traffic. Management frames are kept whole (they hold only the device's own advertised elements).
// Nothing is kept past a capture: the queue is dropped on a new session. Nothing here transmits.
//
// Wire format of one queued record (little-endian), packed back to back in the event's byte string:
//   u32 t_ms   C5 uptime when the frame was heard
//   u8  ch     channel
//   s8  rssi   dBm
//   u8  kind   0 management, 1 data
//   u8  flags  bit 0 = truncated (caplen < origlen)
//   u16 caplen bytes that follow
//   u16 origlen frame length without the FCS
//   u8  data[caplen]
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_REC_HDR   12
#define CAP_MAX_MGMT  400            // management frames larger than this are truncated

void   capture_set_enabled(bool on);               // CYD request; only takes effect with a Wi-Fi focus
void   capture_end(void);                          // the focus ended: drain what is queued, then report inactive
void   capture_abort(void);                        // new session: drop everything now
bool   capture_enabled(void);
void   capture_note(const uint8_t* frame, int length, int rssi, int ch, bool is_mgmt);   // Wi-Fi task
// Link task: copy whole queued records into out (up to cap bytes). *active tells the CYD whether capture is
// still on; *end is true once, after the last record, when capture has finished. *dropped is the running count
// of records lost to a full queue.
size_t capture_pop(uint8_t* out, size_t cap, bool* active, bool* end, uint32_t* dropped);
size_t capture_pending(void);

#ifdef __cplusplus
}
#endif
