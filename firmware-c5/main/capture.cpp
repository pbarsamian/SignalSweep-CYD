// SPDX-License-Identifier: GPL-3.0-or-later
// capture.cpp — see capture.h.
#include "capture.h"
#include "focus.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define RING_SZ 8192

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_ring[RING_SZ];
static size_t  s_head = 0, s_tail = 0, s_used = 0;     // push at head, pop at tail
static volatile bool s_en = false;                      // accepting frames
static volatile bool s_ending = false;                  // stopped: drain the queue, then report the end
static uint32_t s_dropped = 0;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void ring_put(const uint8_t* p, size_t n) {
    size_t first = RING_SZ - s_head < n ? RING_SZ - s_head : n;
    memcpy(s_ring + s_head, p, first);
    if (first < n) memcpy(s_ring, p + first, n - first);
    s_head = (s_head + n) % RING_SZ;
    s_used += n;
}

static void ring_peek(size_t off, uint8_t* out, size_t n) {
    size_t pos = (s_tail + off) % RING_SZ;
    size_t first = RING_SZ - pos < n ? RING_SZ - pos : n;
    memcpy(out, s_ring + pos, first);
    if (first < n) memcpy(out + first, s_ring, n - first);
}

static void reset_locked(void) { s_head = s_tail = s_used = 0; }

bool capture_enabled(void) { return s_en; }

void capture_set_enabled(bool on) {
    if (on) {
        if (s_en || !focus_active()) return;             // a capture needs a focus to say whose frames
        portENTER_CRITICAL(&s_mux);
        reset_locked();
        s_dropped = 0;
        s_ending = false;
        s_en = true;
        portEXIT_CRITICAL(&s_mux);
    } else {
        capture_end();
    }
}

void capture_end(void) {
    if (!s_en) return;
    s_en = false;
    s_ending = true;                                      // the link task reports the end after the last record
}

void capture_abort(void) {
    s_en = false;
    portENTER_CRITICAL(&s_mux);
    reset_locked();
    s_ending = false;
    portEXIT_CRITICAL(&s_mux);
}

// Length of the 802.11 MAC header of a data frame, from its frame-control field.
static int data_hdr_len(const uint8_t* f) {
    uint8_t sub = f[0] >> 4;
    bool to_ds = f[1] & 0x01, from_ds = f[1] & 0x02;
    int h = 24 + ((to_ds && from_ds) ? 6 : 0);
    if (sub & 0x08) {                                     // QoS
        h += 2;
        if (f[1] & 0x80) h += 4;                          // HT control
    }
    return h;
}

void capture_note(const uint8_t* f, int length, int rssi, int ch, bool is_mgmt) {
    if (!s_en || length < 28 || !focus_is_wifi()) return;     // a BLE focus must not pick up a Wi-Fi frame that shares its MAC
    if (!focus_matches(f + 4) && !focus_matches(f + 10) && !focus_matches(f + 16)) return;   // the focused target's frames only
    int orig = length - 4;                                // without the FCS
    int cap = orig;
    uint8_t flags = 0;
    if (is_mgmt) {
        if (cap > CAP_MAX_MGMT) { cap = CAP_MAX_MGMT; flags |= 1; }
    } else {
        int h = data_hdr_len(f);                          // headers only: never the LLC, IP or anything above
        if (h > orig) h = orig;
        if (cap > h) { cap = h; flags |= 1; }
    }
    uint8_t hdr[CAP_REC_HDR];
    uint32_t t = ms_now();
    hdr[0] = (uint8_t)t; hdr[1] = (uint8_t)(t >> 8); hdr[2] = (uint8_t)(t >> 16); hdr[3] = (uint8_t)(t >> 24);
    hdr[4] = (uint8_t)ch;
    hdr[5] = (uint8_t)(int8_t)(rssi < -127 ? -127 : rssi > 127 ? 127 : rssi);
    hdr[6] = is_mgmt ? 0 : 1;
    hdr[7] = flags;
    hdr[8] = (uint8_t)cap; hdr[9] = (uint8_t)(cap >> 8);
    hdr[10] = (uint8_t)orig; hdr[11] = (uint8_t)(orig >> 8);
    portENTER_CRITICAL(&s_mux);
    if (s_en && (size_t)(CAP_REC_HDR + cap) <= RING_SZ - s_used) {
        ring_put(hdr, CAP_REC_HDR);
        ring_put(f, (size_t)cap);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void capture_note_ble(const uint8_t mac[6], uint8_t addr_type, uint8_t evtype, int rssi, const uint8_t* data,
                      size_t len, const uint8_t direct_a[6], uint8_t direct_type) {
    if (!s_en || !focus_is_ble() || !focus_matches(mac) || evtype > 4) return;
    static const uint8_t pdu_of[5] = { 0, 1, 6, 2, 4 };   // NimBLE event type -> link-layer PDU type
    bool direct = evtype == 1;
    if (len > 31) len = 31;                              // legacy advertising data limit
    uint8_t body[12 + 31];
    size_t n = 0;
    for (int i = 0; i < 6; i++) body[n++] = mac[5 - i];  // AdvA, over-the-air order
    if (direct) { memcpy(body + n, direct_a, 6); n += 6; len = 0; }
    if (len) { memcpy(body + n, data, len); n += len; }
    uint8_t hdr[CAP_REC_HDR];
    uint32_t t = ms_now();
    hdr[0] = (uint8_t)t; hdr[1] = (uint8_t)(t >> 8); hdr[2] = (uint8_t)(t >> 16); hdr[3] = (uint8_t)(t >> 24);
    hdr[4] = 0;                                          // the scan does not report the advertising channel
    hdr[5] = (uint8_t)(int8_t)(rssi < -127 ? -127 : rssi > 127 ? 127 : rssi);
    hdr[6] = 2;
    hdr[7] = (uint8_t)(((addr_type & 1) ? 2 : 0) | ((direct && (direct_type & 1)) ? 4 : 0) | (pdu_of[evtype] << 4));
    hdr[8] = (uint8_t)n; hdr[9] = (uint8_t)(n >> 8);
    hdr[10] = (uint8_t)n; hdr[11] = (uint8_t)(n >> 8);
    portENTER_CRITICAL(&s_mux);
    if (s_en && CAP_REC_HDR + n <= RING_SZ - s_used) {
        ring_put(hdr, CAP_REC_HDR);
        ring_put(body, n);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
}

size_t capture_pending(void) { return s_used; }

size_t capture_pop(uint8_t* out, size_t cap, bool* active, bool* end, uint32_t* dropped) {
    size_t n = 0;
    *end = false;
    portENTER_CRITICAL(&s_mux);
    while (s_used >= CAP_REC_HDR) {
        uint8_t h[CAP_REC_HDR];
        ring_peek(0, h, CAP_REC_HDR);
        size_t rec = CAP_REC_HDR + (size_t)(h[8] | (h[9] << 8));
        if (n + rec > cap || rec > s_used) break;
        ring_peek(0, out + n, rec);
        s_tail = (s_tail + rec) % RING_SZ;
        s_used -= rec;
        n += rec;
    }
    if (s_ending && s_used == 0) { s_ending = false; *end = true; }
    *active = s_en;
    *dropped = s_dropped;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
