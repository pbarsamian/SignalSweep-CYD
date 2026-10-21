// SPDX-License-Identifier: GPL-3.0-or-later
// capture.h — targeted frame capture for the CYD's microSD (issue #10). PASSIVE.
//
// While a focus is active and the CYD has switched capture on, every frame (Wi-Fi) or advert (BLE) to or from
// the focused MAC is queued here and streamed to the CYD (SS_EVT_CAPTURE), which writes it to a pcapng file. Data frames are
// cut to their 802.11 header: no LLC, no IP, no payload, so no handshakes (EAPOL is a data frame) and nothing
// of anyone's traffic. Management frames are kept whole (they hold only the device's own advertised elements).
// BLE: the passive scan reports adverts, not link-layer packets, so the CYD rebuilds each advert as a link-layer
// packet (access address, header, AdvA, data). The channel and CRC are not known and are not invented. A passive
// scan never receives scan responses, so none are captured.
// Nothing is kept past a capture: the queue is dropped on a new session. Nothing here transmits.
//
// Wire format of one queued record (little-endian), packed back to back in the event's byte string:
//   u32 t_ms   C5 uptime when the frame was heard
//   u8  ch     channel (0 for BLE: not known)
//   s8  rssi   dBm
//   u8  kind   0 Wi-Fi management, 1 Wi-Fi data, 2 BLE advert
//   u8  flags  bit 0 = truncated (caplen < origlen)
//              kind 2 only: bit 1 = TxAdd (AdvA is random), bit 2 = RxAdd (InitA is random),
//                           bits 4..7 = link-layer PDU type (0 ADV_IND, 1 ADV_DIRECT_IND, 2 ADV_NONCONN_IND,
//                           4 SCAN_RSP, 6 ADV_SCAN_IND)
//   u16 caplen bytes that follow
//   u16 origlen frame length without the FCS (BLE: equals caplen)
//   u8  data[caplen]  Wi-Fi: the 802.11 frame. BLE: AdvA (6 bytes, over-the-air order), then InitA (6 bytes) for
//                     ADV_DIRECT_IND only, then the advertising data
//   If flags bit 3 (0x08) is set an annotation follows the data: u8 conf (summed weight, capped at 255), u8 best
//   (the strongest single signal), u8 rlen, char rule[rlen] (the matched rule name, <= CAP_RULE_MAX, no
//   terminator). It is the signature engine's verdict on that one frame or advert; non-matching ones carry none.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_REC_HDR   12
#define CAP_MAX_MGMT  400            // management frames larger than this are truncated
#define CAP_RULE_MAX  24             // longest rule name carried in an annotation

void   capture_set_enabled(bool on);               // CYD request; only takes effect with a focus
void   capture_end(void);                          // the focus ended: drain what is queued, then report inactive
void   capture_abort(void);                        // new session: drop everything now
bool   capture_enabled(void);
// Wi-Fi task. rule/conf/best: the signature engine's verdict on this frame (rule NULL = no match).
void   capture_note(const uint8_t* frame, int length, int rssi, int ch, bool is_mgmt, const char* rule, int conf, int best);
// Link task: copy whole queued records into out (up to cap bytes). *active tells the CYD whether capture is
// still on; *end is true once, after the last record, when capture has finished. *dropped is the running count
// of records lost to a full queue.
size_t capture_pop(uint8_t* out, size_t cap, bool* active, bool* end, uint32_t* dropped);
size_t capture_pending(void);
// BLE task: one advert from the scan. mac is MSB first (as the registry keeps it); evtype is the NimBLE report
// event type (0..4); adv_a is the address in over-the-air order; direct_a/direct_type only for evtype 1.
void   capture_note_ble(const uint8_t mac[6], uint8_t addr_type, uint8_t evtype, int rssi, const uint8_t* data,
                        size_t len, const uint8_t direct_a[6], uint8_t direct_type, const char* rule, int conf, int best);

#ifdef __cplusplus
}
#endif
