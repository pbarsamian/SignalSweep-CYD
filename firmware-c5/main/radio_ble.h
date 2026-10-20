// SPDX-License-Identifier: GPL-3.0-or-later
// radio_ble.h — passive BLE scanning on the C5 (ESP-IDF NimBLE host).
// PASSIVE means passive (BUILD_MANIFEST invariant 7): the scan is passive, so no SCAN_REQ is ever
// sent. Consequence: a device that carries its name ONLY in a scan response is never named, so
// the name-based rules (flock, flipper, "penguin-NNNNNNNNNN", ...) can miss it. The standalone
// firmware scanned actively to get those names; doing that here would be an ACTIVE operation and
// belongs behind the consent gate.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ss_cbor.h"

void     radio_ble_start(bool enabled);          // init NimBLE; scan only if `enabled`
void     radio_ble_set_enabled(bool on);         // charge mode: stop / resume scanning
bool     radio_ble_enabled(void);
// Recently seen BLE devices (every advert, matched or not), freshest first.
bool     radio_ble_encode_devs(ss_cbor_w_t* w);
void     radio_ble_log_stats(void);
void     radio_ble_set_focus(bool on);           // focus: scan at full duty (window = interval)
