// SPDX-License-Identifier: GPL-3.0-or-later
// detections_api.h — C-callable face of the C++ radio/registry code, so dispatch.c
// (plain C) can answer SCAN_WIFI / LIST_DETECTIONS and stream detection events.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ss_cbor.h"

#ifdef __cplusplus
extern "C" {
#endif

void     det_api_start(bool radio_on);               // registry + passive Wi-Fi capture (radio only if allowed)
void     det_radio_enable(bool on);                  // charge mode: radio fully off / back on
// Detection-type toggles from the CYD's settings (-1 leaves a type unchanged). A type that is
// switched off is also dropped from the registry at once.
void     det_set_config(int drone, int tracker, int camera, int bodycam, int attack);
bool     det_encode_list(ss_cbor_w_t* w, uint32_t offset);   // LIST_DETECTIONS result (one page)
bool     det_encode_aps(ss_cbor_w_t* w);             // SCAN_WIFI result (seen APs)
bool     det_encode_ble(ss_cbor_w_t* w);             // SCAN_BLE result (seen BLE devices)
bool     det_pop_event(ss_cbor_w_t* w);              // next pending detection event, if any
// GET_DEVICE_PROFILE: everything the C5 knows about one detection (by MAC text). False if unknown.
bool     det_encode_profile(const char* mac_text, ss_cbor_w_t* w);
// FOCUS_TARGET {on:bool, mac:text, radio:"wifi"|"ble", ch:uint}: park on one target (or release).
// Returns an SS_ err code; on success the CBOR reply describes the focus.
int      det_focus_request(const uint8_t* args, size_t n, ss_cbor_w_t* w);
void     det_focus_stop(const char* why);            // end any focus and restore the sweep
bool     det_encode_focus(ss_cbor_w_t* w);           // GET_DEVICE_PROFILE view "focus"; false if not focused
bool     det_pop_focus_event(ss_cbor_w_t* w);        // next RSSI_TICK body (rate-limited), if focused
// Scan scheduler (SET_CONFIG keys smode, bpct, wms, bms): -1 leaves a value unchanged.
void     det_set_opennet(bool on);               // open-network profiling (issue #11): default off, off again on every HELLO
void     det_set_sched(int mode, int ble_pct, int wifi_ms, int ble_ms, int band_ms);
uint8_t  det_scan_state(void);                      // what is being scanned now: bits 0-1 run mask (1 Wi-Fi, 2 BLE), bits 4-5 lock, bit 6 = on 5 GHz
uint32_t det_count(void);
void     det_tick(void);                             // housekeeping: prune stale, log stats

#ifdef __cplusplus
}
#endif
