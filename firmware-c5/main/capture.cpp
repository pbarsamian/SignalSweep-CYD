// SPDX-License-Identifier: GPL-3.0-or-later
// capture.cpp — see capture.h.
#include "capture.h"
#include "focus.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include <string.h>

#define RING_SZ      8192                               // the live queue
#define PRE_SZ_BEST  16384                              // pre-trigger ring, tried first
#define PRE_SZ_SMALL 8192                               // and this if the heap cannot spare 16 KB

static const char* TAG = "capture";

struct Ring {
    uint8_t* buf;
    size_t   sz, head, tail, used;                      // push at head, pop at tail
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_main_buf[RING_SZ];
static Ring    s_main = { s_main_buf, RING_SZ, 0, 0, 0 };    // the live queue: what is streamed to the CYD
static Ring    s_pre  = { nullptr, 0, 0, 0, 0 };             // pre-trigger ring: allocated while focused, rolls (oldest overwritten)
static bool    s_pre_frozen = false;                         // a capture has started: the ring is being drained, not filled
static volatile bool s_en = false;                      // accepting frames
static volatile bool s_ending = false;                  // stopped: drain the queue, then report the end
static uint32_t s_dropped = 0;

#define CAP_F_NOTE 0x08                                 // flags bit 3: an annotation follows the data
#define CAP_F_PRE  0x80                                 // flags bit 7: heard before REC (BLE PDU types are <= 6, so it is free there too)

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void ring_put(Ring& r, const uint8_t* p, size_t n) {
    size_t first = r.sz - r.head < n ? r.sz - r.head : n;
    memcpy(r.buf + r.head, p, first);
    if (first < n) memcpy(r.buf, p + first, n - first);
    r.head = (r.head + n) % r.sz;
    r.used += n;
}

static void ring_peek(const Ring& r, size_t off, uint8_t* out, size_t n) {
    size_t pos = (r.tail + off) % r.sz;
    size_t first = r.sz - pos < n ? r.sz - pos : n;
    memcpy(out, r.buf + pos, first);
    if (first < n) memcpy(out + first, r.buf, n - first);
}

static void ring_reset(Ring& r) { r.head = r.tail = r.used = 0; }

// Length of the whole record at the tail (header + data + annotation), or 0 if it is not all there yet.
static size_t rec_len(const Ring& r) {
    if (r.used < CAP_REC_HDR) return 0;
    uint8_t h[CAP_REC_HDR];
    ring_peek(r, 0, h, CAP_REC_HDR);
    size_t rec = CAP_REC_HDR + (size_t)(h[8] | (h[9] << 8));
    if (h[7] & CAP_F_NOTE) {                            // annotation: u8 conf, u8 best, u8 rlen, rule
        if (rec + 3 > r.used) return 0;
        uint8_t rl;
        ring_peek(r, rec + 2, &rl, 1);
        rec += 3 + rl;
    }
    return rec > r.used ? 0 : rec;
}

// Copy whole records from the ring into out (after n bytes already there, up to cap). Returns the new n.
static size_t ring_pop(Ring& r, uint8_t* out, size_t n, size_t cap) {
    for (;;) {
        size_t rec = rec_len(r);
        if (!rec || n + rec > cap) break;
        ring_peek(r, 0, out + n, rec);
        r.tail = (r.tail + rec) % r.sz;
        r.used -= rec;
        n += rec;
    }
    return n;
}

// Take the pre-trigger buffer away (under the lock); the caller frees it outside the lock.
static uint8_t* pre_detach_locked(void) {
    uint8_t* b = s_pre.buf;
    s_pre.buf = nullptr;
    s_pre.sz = 0;
    ring_reset(s_pre);
    s_pre_frozen = false;
    return b;
}

bool capture_enabled(void) { return s_en; }

// A focus has just started: set aside the pre-trigger ring, so the frames heard before REC are not lost.
void capture_focus_begin(void) {
    uint8_t* b = (uint8_t*)malloc(PRE_SZ_BEST);
    size_t sz = PRE_SZ_BEST;
    if (!b) { b = (uint8_t*)malloc(PRE_SZ_SMALL); sz = PRE_SZ_SMALL; }
    if (!b) { ESP_LOGW(TAG, "no memory for a pre-trigger ring: capture starts at REC"); return; }
    portENTER_CRITICAL(&s_mux);
    uint8_t* old = s_pre.buf;
    s_pre.buf = b;
    s_pre.sz = sz;
    ring_reset(s_pre);
    s_pre_frozen = s_en;
    portEXIT_CRITICAL(&s_mux);
    free(old);                                           // normally null
    ESP_LOGI(TAG, "pre-trigger ring %u B", (unsigned)sz);
}

// The focus has ended. With no capture to finish, the ring (which only ever held that target's frames) is freed now.
void capture_focus_end(void) {
    uint8_t* b = nullptr;
    portENTER_CRITICAL(&s_mux);
    if (!s_en && !s_ending) b = pre_detach_locked();     // otherwise capture_pop frees it once it has drained
    portEXIT_CRITICAL(&s_mux);
    free(b);
}

void capture_set_enabled(bool on) {
    if (on) {
        if (s_en || !focus_active()) return;             // a capture needs a focus to say whose frames
        portENTER_CRITICAL(&s_mux);
        ring_reset(s_main);
        s_dropped = 0;
        s_ending = false;
        s_pre_frozen = true;                             // what the ring holds now goes out first, then the live frames
        s_en = true;
        size_t pre = s_pre.buf ? s_pre.used : 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "capture on: %u B of pre-trigger frames queued first", (unsigned)pre);
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
    ring_reset(s_main);
    s_ending = false;
    uint8_t* b = pre_detach_locked();
    portEXIT_CRITICAL(&s_mux);
    free(b);
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

// True when a frame from the focused target should be recorded: a capture is on, or the pre-trigger ring is rolling.
static inline bool listening(void) { return s_en || (s_pre.buf && !s_pre_frozen); }

// Append a record to the pre-trigger ring, overwriting the oldest records to make room.
static void pre_push_locked(const uint8_t* hdr, const uint8_t* data, size_t n, const uint8_t* note, size_t nl) {
    size_t total = CAP_REC_HDR + n + nl;
    if (total > s_pre.sz) return;
    while (s_pre.sz - s_pre.used < total) {
        size_t rec = rec_len(s_pre);
        if (!rec) { ring_reset(s_pre); break; }
        s_pre.tail = (s_pre.tail + rec) % s_pre.sz;
        s_pre.used -= rec;
    }
    uint8_t h[CAP_REC_HDR];
    memcpy(h, hdr, CAP_REC_HDR);
    h[7] |= CAP_F_PRE;                                    // the CYD labels these: they were heard before REC
    ring_put(s_pre, h, CAP_REC_HDR);
    ring_put(s_pre, data, n);
    if (nl) ring_put(s_pre, note, nl);
}

// Queue one record: header, data, and the optional annotation. With a capture on it goes to the live queue (dropped and
// counted if that is full); otherwise it rolls into the pre-trigger ring.
static void queue_record(uint8_t* hdr, const uint8_t* data, size_t n, const char* rule, int conf, int best) {
    uint8_t note[3 + CAP_RULE_MAX];
    size_t nl = 0;
    if (rule && rule[0]) {
        size_t rl = strlen(rule);
        if (rl > CAP_RULE_MAX) rl = CAP_RULE_MAX;
        note[0] = (uint8_t)(conf < 0 ? 0 : conf > 255 ? 255 : conf);
        note[1] = (uint8_t)(best < 0 ? 0 : best > 255 ? 255 : best);
        note[2] = (uint8_t)rl;
        memcpy(note + 3, rule, rl);
        nl = 3 + rl;
        hdr[7] |= CAP_F_NOTE;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_en) {
        if (CAP_REC_HDR + n + nl <= s_main.sz - s_main.used) {
            ring_put(s_main, hdr, CAP_REC_HDR);
            ring_put(s_main, data, n);
            if (nl) ring_put(s_main, note, nl);
        } else {
            s_dropped++;
        }
    } else if (s_pre.buf && !s_pre_frozen) {
        pre_push_locked(hdr, data, n, note, nl);
    }
    portEXIT_CRITICAL(&s_mux);
}

void capture_note(const uint8_t* f, int length, int rssi, int ch, bool is_mgmt, const char* rule, int conf, int best) {
    if (!listening() || length < 28 || !focus_is_wifi()) return;     // a BLE focus must not pick up a Wi-Fi frame that shares its MAC
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
    queue_record(hdr, f, (size_t)cap, rule, conf, best);
}

void capture_note_ble(const uint8_t mac[6], uint8_t addr_type, uint8_t evtype, int rssi, const uint8_t* data,
                      size_t len, const uint8_t direct_a[6], uint8_t direct_type, const char* rule, int conf, int best) {
    if (!listening() || !focus_is_ble() || !focus_matches(mac) || evtype > 4) return;
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
    queue_record(hdr, body, n, rule, conf, best);
}

size_t capture_pending(void) { return s_main.used + (s_pre_frozen ? s_pre.used : 0); }

size_t capture_pop(uint8_t* out, size_t cap, bool* active, bool* end, uint32_t* dropped) {
    size_t n = 0;
    uint8_t* to_free = nullptr;
    *end = false;
    portENTER_CRITICAL(&s_mux);
    if (s_pre.buf && s_pre_frozen) n = ring_pop(s_pre, out, n, cap);          // the pre-trigger frames go first: they are older
    if (!s_pre.buf || !s_pre_frozen || s_pre.used == 0) n = ring_pop(s_main, out, n, cap);
    if (s_ending && s_main.used == 0 && (!s_pre.buf || s_pre.used == 0)) {
        s_ending = false;
        *end = true;
        if (focus_active()) {                                                 // still focused: the ring starts rolling again for the next REC
            ring_reset(s_pre);
            s_pre_frozen = false;
        } else {
            to_free = pre_detach_locked();
        }
    }
    *active = s_en;
    *dropped = s_dropped;
    portEXIT_CRITICAL(&s_mux);
    free(to_free);
    return n;
}
