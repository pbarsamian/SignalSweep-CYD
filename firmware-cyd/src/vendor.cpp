// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep CYD — vendor and protocol name lookups
#include "vendor.h"
#include "vendor_db_data.h"

bool vendor_db_full() { return VENDOR_DB_FULL; }

template <typename K>
static int bsearch_key(const K* keys, int n, K want) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid] == want) return mid;
        if (keys[mid] < want) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char* vendor_for_mac(const uint8_t m[6]) {
    uint64_t v = ((uint64_t)m[0] << 40) | ((uint64_t)m[1] << 32) | ((uint64_t)m[2] << 24) |
                 ((uint64_t)m[3] << 16) | ((uint64_t)m[4] << 8) | m[5];
    int i;
    // Longest registered block first: MA-S (36) > MA-M (28) > MA-L (24)
    if (OUI36_COUNT && (i = bsearch_key<uint64_t>(OUI36_KEY, OUI36_COUNT, v >> 12)) >= 0)
        return VENDOR_NAMES + OUI36_NAME[i];
    if (OUI28_COUNT && (i = bsearch_key<uint32_t>(OUI28_KEY, OUI28_COUNT, (uint32_t)(v >> 20))) >= 0)
        return VENDOR_NAMES + OUI28_NAME[i];
    if (OUI24_COUNT && (i = bsearch_key<uint32_t>(OUI24_KEY, OUI24_COUNT, (uint32_t)(v >> 24))) >= 0)
        return VENDOR_NAMES + OUI24_NAME[i];
    return nullptr;
}

const char* vendor_for_company(uint16_t cid) {
    int i = bsearch_key<uint16_t>(CID_KEY, CID_COUNT, cid);
    return i >= 0 ? VENDOR_NAMES + CID_NAME[i] : nullptr;
}

const char* name_for_uuid16(uint16_t u) {
    int i = bsearch_key<uint16_t>(UUID16_KEY, UUID16_COUNT, u);
    return i >= 0 ? VENDOR_NAMES + UUID16_NAME[i] : nullptr;
}

// Port of upstream appleRole() (Continuity types, furiousMAC facts).
// For 0x12, `info` is the payload length byte: 0x19 = separated (tracker form).
// For 0x10, `info` is the packed status/activity byte.
const char* apple_role(uint8_t type, uint8_t info) {
    switch (type) {
        case 0x10:
            switch (info & 0x0F) {
                case 0x01: return "iPhone/iPad/Mac (reporting off)";
                case 0x03: return "iPhone/iPad/Mac (idle)";
                case 0x05: return "iPhone/iPad/Mac (audio, locked)";
                case 0x07: return "iPhone/iPad/Mac (active)";
                case 0x09: return "iPhone/iPad/Mac (video playing)";
                case 0x0A: return "Watch (on wrist, unlocked)";
                case 0x0B: return "iPhone/iPad/Mac (just used)";
                case 0x0D: return "iPhone (driving)";
                case 0x0E: return "iPhone (on a call)";
                default:   return "iPhone/iPad/Mac";
            }
        case 0x12: return info == 0x19 ? "Find My (separated from owner)" : "Find My (with owner)";
        case 0x02: return "iBeacon";
        case 0x03: return "AirPrint (printer)";
        case 0x05: return "AirDrop";
        case 0x06: return "HomeKit accessory";
        case 0x07: return "AirPods pairing";
        case 0x08: return "Hey Siri";
        case 0x09: return "AirPlay target (TV/HomePod/Mac)";
        case 0x0A: return "AirPlay source";
        case 0x0B: return "Watch (Magic Switch)";
        case 0x0C: return "Handoff";
        case 0x0D: return "Tethering target";
        case 0x0E: return "Tethering source (hotspot)";
        case 0x0F: return "Nearby Action";
        default:   return "Apple device";
    }
}

// Bluetooth Appearance: category = value >> 6
const char* ble_appearance(uint16_t a) {
    static const char* const CAT[] = {
        "Unknown", "Phone", "Computer", "Watch", "Clock", "Display", "Remote control",
        "Eyeglasses", "Tag", "Keyring", "Media player", "Barcode scanner", "Thermometer",
        "Heart rate sensor", "Blood pressure", "HID (keyboard/mouse)", "Glucose meter",
        "Running/walking sensor", "Cycling sensor" };
    uint16_t c = a >> 6;
    return c < sizeof(CAT)/sizeof(CAT[0]) ? CAT[c] : nullptr;
}

// ASTM F3411 UA type
const char* odid_ua_type(uint8_t t) {
    static const char* const T[] = {
        "Not declared", "Aeroplane", "Helicopter / multirotor", "Gyroplane",
        "Hybrid lift (VTOL)", "Ornithopter", "Glider", "Kite", "Free balloon",
        "Captive balloon", "Airship", "Free fall / parachute", "Rocket",
        "Tethered powered aircraft", "Ground obstacle", "Other" };
    return t < 16 ? T[t] : "Unknown";
}
