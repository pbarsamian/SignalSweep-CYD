// SPDX-License-Identifier: GPL-3.0-or-later
// radio_wifi.h — passive dual-band Wi-Fi capture on the C5.
// PASSIVE means passive (BUILD_MANIFEST invariant 7): promiscuous receive only. No
// probe requests, no esp_wifi_scan_start() (its default scan type is ACTIVE), no
// transmit of any kind.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ss_cbor.h"

void     radio_wifi_start(bool enabled);         // init + hop task; radio on only if `enabled`
void     radio_wifi_set_enabled(bool on);        // charge mode switches the radio fully off/on
// Seen access points (beacons / probe responses), freshest first.
bool     radio_wifi_encode_aps(ss_cbor_w_t* w);
void     radio_wifi_log_stats(void);             // one summary line on the console
uint32_t radio_wifi_ap_count(void);
