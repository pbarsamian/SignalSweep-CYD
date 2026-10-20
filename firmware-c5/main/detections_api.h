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
bool     det_encode_list(ss_cbor_w_t* w, uint32_t offset);   // LIST_DETECTIONS result (one page)
bool     det_encode_aps(ss_cbor_w_t* w);             // SCAN_WIFI result (seen APs)
bool     det_pop_event(ss_cbor_w_t* w);              // next pending detection event, if any
uint32_t det_count(void);
void     det_tick(void);                             // housekeeping: prune stale, log stats

#ifdef __cplusplus
}
#endif
