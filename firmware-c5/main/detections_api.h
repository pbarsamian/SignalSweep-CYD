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
uint32_t det_count(void);
void     det_tick(void);                             // housekeeping: prune stale, log stats

#ifdef __cplusplus
}
#endif
