// SPDX-License-Identifier: GPL-3.0-or-later
// focus.h — focus mode on the C5: work ONE target instead of sweeping (port of the standalone
// firmware's focus.cpp, GPL-3.0-or-later, from B0z0dcl0wn/SignalSweep's lineage).
//
// PASSIVE only. Focus parks the radio: Wi-Fi locks the target's channel (and also receives data
// frame HEADERS for peer / traffic tallies; payloads are never read or stored), BLE scans at full
// duty. Nothing here transmits.
//
// Fail-safe: focus is a lease. The CYD must refresh it (FOCUS_TARGET on) every few seconds;
// without a refresh it lapses after FOCUS_TTL_MS. It also ends on a new session (HELLO), a stale
// CYD shadow, charge mode, and an explicit off.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOCUS_TTL_MS     15000
#define FOCUS_MAX_PEERS  12
#define FOCUS_MAX_VIE    8
#define FOCUS_RAW_MAX    62

#define PEER_UNKNOWN 0
#define PEER_CLIENT  1       // peer is a client of the target (target acts as the AP)
#define PEER_AP      2       // peer is the AP the target talks to (target is a client)

struct FocusPeer {
    uint8_t  mac[6] = {};
    uint8_t  role = PEER_UNKNOWN;
    int8_t   rssi = -127;             // of the peer's own transmissions
    uint32_t to_frames = 0, from_frames = 0;     // target -> peer, peer -> target
    uint32_t to_bytes = 0, from_bytes = 0;
    uint32_t qos_frames = 0;
    uint32_t last_ms = 0;
};

struct FocusState {
    bool     active = false;
    bool     wifi = false;            // else BLE
    uint8_t  mac[6] = {};
    uint8_t  channel = 0;             // Wi-Fi only
    uint32_t started_ms = 0, lease_ms = 0;
    uint32_t last_tx_ms = 0;          // last frame / advert from the target
    int8_t   last_rssi = -127;
    uint32_t frames = 0;              // Wi-Fi: frames from the target
    uint32_t beacons = 0, probe_resps = 0, probe_reqs = 0, other_mgmt = 0, data_frames = 0;
    uint32_t adv_count = 0, adv_int_min = 0, last_adv_ms = 0;   // BLE
    uint8_t  raw[FOCUS_RAW_MAX] = {};  uint8_t raw_len = 0;     // BLE advert payload
    uint8_t  ie_map[32] = {};         // Wi-Fi: element IDs seen in the target's own frames
    uint8_t  vie[FOCUS_MAX_VIE][4] = {};  uint8_t vie_count = 0;
    uint8_t  ht_streams = 0;
    uint8_t  peer_count = 0;
    FocusPeer peers[FOCUS_MAX_PEERS];
};

void focus_start(const uint8_t mac[6], bool wifi, uint8_t channel);
void focus_stop(const char* why);
void focus_refresh(void);                          // renew the lease
bool focus_active(void);
bool focus_is_wifi(void);                          // active AND Wi-Fi
bool focus_is_ble(void);                           // active AND BLE
uint8_t focus_channel(void);                       // 0 unless a Wi-Fi focus is active
bool focus_matches(const uint8_t mac[6]);
bool focus_snapshot(FocusState* out);              // false if not active
bool focus_expired(uint32_t now_ms);               // lease lapsed

// From the radio callbacks (cheap; they return at once when not focused)
void focus_note_ble(const uint8_t mac[6], int rssi, const uint8_t* data, size_t len);
void focus_note_wifi_mgmt(const uint8_t* frame, int length, int rssi);   // any mgmt frame
void focus_note_wifi_data(const uint8_t* frame, int length, int rssi);   // data frame header only
