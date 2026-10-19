// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Arduino.h>

bool        vendor_db_full();                       // true once generator has run
const char* vendor_for_mac(const uint8_t mac[6]);   // IEEE MA-S/MA-M/MA-L, or nullptr
const char* vendor_for_company(uint16_t cid);       // BT SIG company ID, or nullptr
const char* name_for_uuid16(uint16_t uuid);         // BT SIG 16-bit UUID, or nullptr
const char* apple_role(uint8_t type, uint8_t info); // Apple Continuity decode
const char* ble_appearance(uint16_t appearance);    // category from Appearance value
const char* odid_ua_type(uint8_t t);                // ASTM UA type
