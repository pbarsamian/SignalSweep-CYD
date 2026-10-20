// SPDX-License-Identifier: GPL-3.0-or-later
// focus.cpp — see focus.h. Ported from the standalone firmware's focus.cpp (peer / traffic tallies,
// IE map, vendor IEs, HT streams, BLE advert timing). Header fields only: no payload is kept.
#include "focus.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "registry.h"

static const char* TAG = "focus";

static FocusState s_f;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_active = false;          // lock-free fast check for the callbacks
static volatile bool s_wifi = false;
static volatile uint8_t s_chan = 0;

static inline int8_t r8(int rssi) { return (int8_t)SS_CONSTRAIN(rssi, -127, 0); }

void focus_start(const uint8_t mac[6], bool wifi, uint8_t channel) {
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_mux);
    s_f = FocusState();
    memcpy(s_f.mac, mac, 6);
    s_f.wifi = wifi;
    s_f.channel = wifi ? channel : 0;
    s_f.started_ms = now;
    s_f.lease_ms = now;
    s_f.active = true;
    s_wifi = wifi;
    s_chan = s_f.channel;
    s_active = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "start %02X:%02X:%02X:%02X:%02X:%02X %s ch%u", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             wifi ? "wifi" : "ble", (unsigned)channel);
}

void focus_stop(const char* why) {
    bool was;
    portENTER_CRITICAL(&s_mux);
    was = s_f.active;
    s_f.active = false;
    s_active = false;
    s_wifi = false;
    s_chan = 0;
    portEXIT_CRITICAL(&s_mux);
    if (was) ESP_LOGI(TAG, "stop (%s), sweeping resumes", why ? why : "?");
}

void focus_refresh(void) {
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_mux);
    s_f.lease_ms = now;
    portEXIT_CRITICAL(&s_mux);
}

bool focus_active(void) { return s_active; }
bool focus_is_wifi(void) { return s_active && s_wifi; }
bool focus_is_ble(void) { return s_active && !s_wifi; }
uint8_t focus_channel(void) { return s_active ? s_chan : 0; }

bool focus_matches(const uint8_t mac[6]) {
    bool m;
    portENTER_CRITICAL(&s_mux);
    m = s_f.active && memcmp(s_f.mac, mac, 6) == 0;
    portEXIT_CRITICAL(&s_mux);
    return m;
}

bool focus_snapshot(FocusState* out) {
    portENTER_CRITICAL(&s_mux);
    bool a = s_f.active;
    if (a) *out = s_f;
    portEXIT_CRITICAL(&s_mux);
    return a;
}

bool focus_expired(uint32_t now_ms) {
    portENTER_CRITICAL(&s_mux);
    bool e = s_f.active && (now_ms - s_f.lease_ms) > FOCUS_TTL_MS;
    portEXIT_CRITICAL(&s_mux);
    return e;
}

// ---- BLE ------------------------------------------------------------------------------------------
void focus_note_ble(const uint8_t mac[6], int rssi, const uint8_t* data, size_t len) {
    if (!s_active || s_wifi) return;
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_mux);
    if (s_f.active && !s_f.wifi && memcmp(s_f.mac, mac, 6) == 0) {
        s_f.adv_count++;
        if (s_f.last_adv_ms) {
            uint32_t gap = now - s_f.last_adv_ms;
            if (gap >= 15 && (s_f.adv_int_min == 0 || gap < s_f.adv_int_min)) s_f.adv_int_min = gap;   // shorter gaps are scan responses
        }
        s_f.last_adv_ms = now;
        s_f.last_tx_ms = now;
        s_f.last_rssi = r8(rssi);
        if (data && len) {
            if (len > FOCUS_RAW_MAX) len = FOCUS_RAW_MAX;
            memcpy(s_f.raw, data, len);
            s_f.raw_len = (uint8_t)len;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

// ---- Wi-Fi ----------------------------------------------------------------------------------------
// Own-frame IE walk (beacon / probe response / probe request of the target)
static void walk_ies(const uint8_t* f, int length, uint8_t sub) {
    int off = (sub == 4) ? 24 : 36;
    int blen = length - off - 4;                          // -4 FCS
    const uint8_t* b = f + off;
    int i = 0;
    while (blen > 0 && i + 2 <= blen) {
        uint8_t id = b[i], el = b[i + 1];
        if (i + 2 + el > blen) break;
        const uint8_t* e = b + i + 2;
        s_f.ie_map[id >> 3] |= (uint8_t)(1 << (id & 7));
        if (id == 221 && el >= 4) {
            bool have = false;
            for (int k = 0; k < s_f.vie_count; k++) if (memcmp(s_f.vie[k], e, 4) == 0) have = true;
            if (!have && s_f.vie_count < FOCUS_MAX_VIE) memcpy(s_f.vie[s_f.vie_count++], e, 4);
        }
        if (id == 45 && el >= 7) {                        // HT capabilities: RX MCS bytes 3..6 = streams 1..4
            uint8_t n = 0;
            for (int k = 3; k <= 6; k++) if (e[k]) n++;
            if (n > s_f.ht_streams) s_f.ht_streams = n;
        }
        i += 2 + el;
    }
}

static void note_peer_locked(bool is_data, const uint8_t* f, int length, int rssi) {
    const uint8_t* ra = f + 4;                            // receiver address
    const uint8_t* ta = f + 10;                           // transmitter address
    bool from_t = memcmp(ta, s_f.mac, 6) == 0;
    bool to_t = memcmp(ra, s_f.mac, 6) == 0;
    if (!from_t && !to_t) return;
    const uint8_t* peer = from_t ? ra : ta;
    bool peer_ok = !(peer[0] & 1) && memcmp(peer, s_f.mac, 6) != 0;      // not group-addressed

    uint8_t sub = f[0] >> 4;
    bool ds_to = f[1] & 0x01, ds_from = f[1] & 0x02;
    uint8_t role = PEER_UNKNOWN;
    if (is_data) {
        if (ds_to && !ds_from) role = from_t ? PEER_AP : PEER_CLIENT;          // station -> AP
        else if (!ds_to && ds_from) role = from_t ? PEER_CLIENT : PEER_AP;     // AP -> station
    } else if (sub == 0 || sub == 2) {                    // (re)association request: client -> AP
        role = from_t ? PEER_AP : PEER_CLIENT;
    } else if (sub == 1 || sub == 3) {                    // (re)association response: AP -> client
        role = from_t ? PEER_CLIENT : PEER_AP;
    }

    uint32_t now = ms_now();
    if (is_data) s_f.data_frames++;
    if (from_t) { s_f.last_tx_ms = now; s_f.last_rssi = r8(rssi); }
    if (!peer_ok) return;
    int slot = -1;
    for (int i = 0; i < s_f.peer_count; i++) if (memcmp(s_f.peers[i].mac, peer, 6) == 0) { slot = i; break; }
    if (slot < 0) {
        if (s_f.peer_count < FOCUS_MAX_PEERS) slot = s_f.peer_count++;
        else {                                            // table full: replace the stalest peer
            slot = 0;
            for (int i = 1; i < FOCUS_MAX_PEERS; i++) if (s_f.peers[i].last_ms < s_f.peers[slot].last_ms) slot = i;
        }
        s_f.peers[slot] = FocusPeer();
        memcpy(s_f.peers[slot].mac, peer, 6);
    }
    FocusPeer& p = s_f.peers[slot];
    p.last_ms = now;
    if (from_t) { p.to_frames++; p.to_bytes += (uint32_t)length; }
    else { p.from_frames++; p.from_bytes += (uint32_t)length; p.rssi = r8(rssi); }
    if (is_data && (sub & 0x08)) p.qos_frames++;
    if (role != PEER_UNKNOWN) p.role = role;
}

void focus_note_wifi_mgmt(const uint8_t* f, int length, int rssi) {
    if (!s_active || !s_wifi || length < 24) return;
    portENTER_CRITICAL(&s_mux);
    if (s_f.active && s_f.wifi) {
        uint8_t sub = (f[0] >> 4) & 0x0F;
        if (memcmp(f + 10, s_f.mac, 6) == 0) {            // the target transmitted this
            s_f.last_tx_ms = ms_now();
            s_f.last_rssi = r8(rssi);
            s_f.frames++;
            if (sub == 8) s_f.beacons++;
            else if (sub == 5) s_f.probe_resps++;
            else if (sub == 4) s_f.probe_reqs++;
            else s_f.other_mgmt++;
            if ((sub == 4 || sub == 5 || sub == 8) && length > 36) walk_ies(f, length, sub);
        }
        note_peer_locked(false, f, length, rssi);
    }
    portEXIT_CRITICAL(&s_mux);
}

void focus_note_wifi_data(const uint8_t* f, int length, int rssi) {
    if (!s_active || !s_wifi || length < 24) return;
    portENTER_CRITICAL(&s_mux);
    if (s_f.active && s_f.wifi) note_peer_locked(true, f, length, rssi);
    portEXIT_CRITICAL(&s_mux);
}
