// SPDX-License-Identifier: GPL-3.0-or-later
// sdcap.cpp — see sdcap.h.
#include "sdcap.h"
#include <SD.h>
#include <SPI.h>
#include "board_pins.h"
#include "ss_cbor.h"
#include "model.h"
#include "vendor.h"

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

#define CAP_REC_HDR 12            // matches firmware-c5/main/capture.h
#define CAP_F_NOTE  0x08          // flags bit 3: an annotation (rule verdict) follows the data
#define STOP_WAIT_MS 4000

static bool     s_mounted = false;
static uint32_t s_try_ms = 0;
static bool     s_tried = false;
static char     s_card[28] = "not checked";

static SdCapState s_state = SDC_IDLE;
static SdCapStats s_st;
static char       s_msg[56] = "";
static File       s_f;
static uint8_t    s_stage[4096];
static size_t     s_sl = 0;
static uint32_t   s_t0 = 0;
static bool       s_have_t0 = false;
static uint32_t   s_last_flush = 0, s_stop_ms = 0;
static char       s_mac[18] = {}, s_label[33] = {};
static uint8_t    s_ch = 0;
static bool       s_ble = false;
static bool       s_failed = false;

SdCapState sdcap_state(void) { return s_state; }
bool sdcap_wanted(void) { return s_state == SDC_REC; }
const SdCapStats& sdcap_stats(void) { return s_st; }
const char* sdcap_last_msg(void) { return s_msg; }
const char* sdcap_card_text(void) { return s_card; }

bool sdcap_card_ok(void) {
    if (s_mounted) return true;
    if (s_tried && millis() - s_try_ms < 3000) return false;      // do not hammer an empty slot
    s_tried = true;
    s_try_ms = millis();
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, SPI, 10000000) || SD.cardType() == CARD_NONE) {
        snprintf(s_card, sizeof(s_card), "no card");
        return false;
    }
    SD.mkdir("/signalsweep");
    snprintf(s_card, sizeof(s_card), "ready %.1f GB", (double)SD.cardSize() / (1024.0 * 1024.0 * 1024.0));
    s_mounted = true;
    return true;
}

// ---- staged writer ---------------------------------------------------------------------------------
static void fail(const char* msg) {
    s_failed = true;
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
}

static void stage_flush(void) {
    if (!s_sl) return;
    if (s_f && !s_failed) {
        if (s_f.write(s_stage, s_sl) != s_sl) fail("SD write failed");
    }
    s_sl = 0;
}

static void put(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    s_st.bytes += (uint32_t)n;
    while (n) {
        size_t room = sizeof(s_stage) - s_sl;
        size_t c = n < room ? n : room;
        memcpy(s_stage + s_sl, b, c);
        s_sl += c; b += c; n -= c;
        if (s_sl == sizeof(s_stage)) stage_flush();
    }
}
static void p16(uint16_t v) { put(&v, 2); }
static void p32(uint32_t v) { put(&v, 4); }
static void pad4(size_t len) { static const uint8_t z[4] = {0, 0, 0, 0}; if (len & 3) put(z, 4 - (len & 3)); }
static size_t opt_size(size_t len) { return 4 + ((len + 3) & ~(size_t)3); }
static void opt_str(uint16_t code, const char* s) { size_t l = strlen(s); p16(code); p16((uint16_t)l); put(s, l); pad4(l); }

static void write_headers(const char* desc) {
    const char* ifname = s_ble ? "ble-focus" : "wlan-focus";
    char app[56];
    snprintf(app, sizeof(app), "SignalSweep CYD %s", FW_VERSION);
    const char* cmt = s_ble
        ? "Timestamps are microseconds since the first advert; the device has no clock. Passive scan: adverts only, rebuilt as link-layer packets; the CRC is not known (zeros) and no scan responses are heard."
        : "Timestamps are microseconds since the first frame; the device has no clock. Passive capture.";
    // Section Header Block
    uint32_t shb = 32 + (uint32_t)(opt_size(strlen(app)) + opt_size(strlen(cmt)));
    p32(0x0A0D0D0A); p32(shb); p32(0x1A2B3C4D); p16(1); p16(0);
    p32(0xFFFFFFFF); p32(0xFFFFFFFF);                              // section length unknown
    opt_str(4, app);                                               // shb_userappl
    opt_str(1, cmt);                                               // opt_comment
    p16(0); p16(0);                                                // opt_endofopt
    p32(shb);
    // Interface Description Block: linktype 127 = 802.11 with a radiotap header, 251 = BLE link-layer packets
    uint32_t idb = 24 + (uint32_t)(opt_size(strlen(ifname)) + opt_size(strlen(desc)) + 8);
    p32(1); p32(idb); p16(s_ble ? 251 : 127); p16(0); p32(0);      // snaplen 0 = no limit
    opt_str(2, ifname);                                            // if_name
    opt_str(3, desc);                                              // if_description
    p16(9); p16(1); { uint8_t v[4] = {6, 0, 0, 0}; put(v, 4); }    // if_tsresol = 10^-6 s
    p16(0); p16(0);
    p32(idb);
}

// cm: the signature engine's verdict on this frame (packet comment), or null when nothing matched.
static void write_epb(uint32_t t_ms, uint8_t ch, int8_t rssi, const uint8_t* fr, uint16_t caplen, uint16_t origlen, const char* cm) {
    if (!s_have_t0) { s_have_t0 = true; s_t0 = t_ms; }
    uint64_t us = (uint64_t)(uint32_t)(t_ms - s_t0) * 1000ULL;
    uint16_t freq = ch <= 13 ? (uint16_t)(2412 + 5 * (ch - 1)) : ch == 14 ? 2484 : (uint16_t)(5000 + 5 * ch);
    uint16_t cflags = ch <= 14 ? 0x0080 : 0x0100;                  // 2 GHz / 5 GHz spectrum
    uint8_t rt[15] = { 0, 0, 15, 0, 0x2A, 0, 0, 0,                 // version, pad, length, present: flags + channel + dBm signal
                       0x00, 0x00,                                 // flags (no FCS), pad to align the channel
                       (uint8_t)freq, (uint8_t)(freq >> 8), (uint8_t)cflags, (uint8_t)(cflags >> 8),
                       (uint8_t)rssi };
    uint32_t cap = 15 + caplen, orig = 15 + origlen;
    uint32_t total = 32 + ((cap + 3) & ~3u) + (cm ? (uint32_t)opt_size(strlen(cm)) + 4 : 0);
    p32(6); p32(total); p32(0); p32((uint32_t)(us >> 32)); p32((uint32_t)us); p32(cap); p32(orig);
    put(rt, sizeof(rt));
    put(fr, caplen);
    pad4(cap);
    if (cm) { opt_str(1, cm); p16(0); p16(0); }                          // opt_comment, opt_endofopt
    p32(total);
    s_st.frames++;
}

// One BLE advert as a link-layer packet (linktype 251): access address, header, AdvA [InitA] data, CRC.
// The scan reports adverts, not packets, so the header is rebuilt from the report and the CRC is zeros.
// rec_flags: bit 1 TxAdd, bit 2 RxAdd, bits 4..7 PDU type (see firmware-c5/main/capture.h).
static void write_ble_epb(uint32_t t_ms, int8_t rssi, uint8_t rec_flags, const uint8_t* body, uint16_t blen, const char* verdict) {
    if (!s_have_t0) { s_have_t0 = true; s_t0 = t_ms; }
    uint64_t us = (uint64_t)(uint32_t)(t_ms - s_t0) * 1000ULL;
    uint8_t pdu[4 + 2 + 43 + 3] = { 0xD6, 0xBE, 0x89, 0x8E };            // advertising access address 0x8E89BED6, LSB first
    if (blen > 43) blen = 43;
    pdu[4] = (uint8_t)((rec_flags >> 4) | ((rec_flags & 2) ? 0x40 : 0) | ((rec_flags & 4) ? 0x80 : 0));
    pdu[5] = (uint8_t)blen;
    memcpy(pdu + 6, body, blen);
    uint32_t cap = 6 + blen + 3;                                         // the 3 CRC bytes stay zero
    char cm[96];
    snprintf(cm, sizeof(cm), "RSSI %d dBm%s%s", (int)rssi, verdict ? "; " : "", verdict ? verdict : "");
    uint32_t total = 32 + ((cap + 3) & ~3u) + (uint32_t)opt_size(strlen(cm)) + 4;
    p32(6); p32(total); p32(0); p32((uint32_t)(us >> 32)); p32((uint32_t)us); p32(cap); p32(cap);
    put(pdu, cap);
    pad4(cap);
    opt_str(1, cm);                                                      // opt_comment: the scan's RSSI
    p16(0); p16(0);                                                      // opt_endofopt
    p32(total);
    s_st.frames++;
}

// ---- sidecar and file lifecycle ----------------------------------------------------------------------
static void sanitize(char* d, size_t cap, const char* s) {
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) {
        char c = *s;
        d[k++] = (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
    }
    d[k] = 0;
}

static void lower_mac(char* d, const char* s) {
    size_t k = 0;
    for (; s[k] && k < 17; k++) d[k] = (char)tolower((unsigned char)s[k]);
    d[k] = 0;
}

static void write_sidecar(void) {
    char path[48];
    snprintf(path, sizeof(path), "%s", s_st.file);
    char* dot = strrchr(path, '.');
    if (!dot) return;
    strcpy(dot, ".txt");
    File t = SD.open(path, FILE_WRITE);
    if (!t) return;
    char lm[20];
    lower_mac(lm, s_mac);
    t.printf("SignalSweep CYD capture %s\r\n\r\n", s_st.file);
    t.printf("Target:   %s  %s\r\n", s_mac, s_label);
    if (s_ble) t.printf("Radio:    BLE (the scan does not report the advertising channel)\r\n");
    else t.printf("Channel:  %u (%s)\r\n", s_ch, s_ch >= 36 ? "5 GHz" : "2.4 GHz");
    t.printf("Frames:   %lu written, %lu dropped by the C5 (its queue was full)\r\n", (unsigned long)s_st.frames, (unsigned long)s_st.dropped);
    t.printf("Firmware: CYD %s\r\n\r\n", FW_VERSION);
    if (s_ble) {
        t.printf("Timestamps are microseconds since the first advert: the device has no clock.\r\n");
        t.printf("Passive scan: only the target's adverts are here, rebuilt as link-layer packets (header from the scan report,\r\n");
        t.printf("access address 0x8E89BED6). The CRC is zeros, so Wireshark shows it as unverified. A passive scan never\r\n");
        t.printf("receives scan responses. Each packet's comment holds the RSSI the C5 measured and, when a signature rule matched\r\n");
        t.printf("that advert, the rule and its confidence.\r\n\r\n");
        t.printf("Wireshark display filter (adverts from the target):\r\n  btle.advertising_address == %s\r\n", lm);
        t.close();
        return;
    }
    t.printf("Timestamps are microseconds since the first frame: the device has no clock.\r\n");
    t.printf("Data frames are cut to the 802.11 header: no payloads, no handshakes. Management frames are whole.\r\n\r\n");
    t.printf("Wireshark display filter (frames to or from the target):\r\n  wlan.addr == %s\r\n\r\n", lm);
    t.printf("Name map: save these lines as a file named 'ethers' in Wireshark's personal configuration folder\r\n");
    t.printf("(Help > About Wireshark > Folders), then turn on View > Name Resolution > Resolve Physical Addresses.\r\n");
    char nm[40];
    sanitize(nm, sizeof(nm), s_label[0] ? s_label : "target");
    t.printf("%s\t%s\r\n", lm, nm);
    for (int i = 0; i < g_fv.npeers; i++) {
        uint8_t m[6];
        unsigned b[6];
        if (sscanf(g_fv.peers[i].mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
        for (int k = 0; k < 6; k++) m[k] = (uint8_t)b[k];
        const char* v = (m[0] & 0x02) ? nullptr : vendor_for_mac(m);     // randomised addresses carry no vendor
        if (!v) continue;
        char pn[40], pm[20];
        sanitize(pn, sizeof(pn), v);
        lower_mac(pm, g_fv.peers[i].mac);
        t.printf("%s\t%s\r\n", pm, pn);
    }
    t.close();
}

static void finish(const char* msg) {
    if (s_state == SDC_IDLE) return;
    stage_flush();
    if (s_f) { s_f.flush(); s_f.close(); }
    if (!s_failed) write_sidecar();
    if (!s_failed) snprintf(s_msg, sizeof(s_msg), "%s: %lu frames", msg, (unsigned long)s_st.frames);
    s_state = SDC_IDLE;
    Serial.printf("[cap] %s (%s, %lu frames, %lu bytes, %lu dropped)\n", s_msg, s_st.file, (unsigned long)s_st.frames,
                  (unsigned long)s_st.bytes, (unsigned long)s_st.dropped);
}

bool sdcap_start(const char* mac, const char* label, uint8_t ch, bool ble) {
    if (s_state != SDC_IDLE) return false;
    if (!sdcap_card_ok()) { snprintf(s_msg, sizeof(s_msg), "no SD card"); return false; }
    int n = 1;
    char path[40];
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), "/signalsweep/cap_%03d.pcapng", n);
        if (!SD.exists(path)) break;
    }
    if (n >= 1000) { snprintf(s_msg, sizeof(s_msg), "no free file name"); return false; }
    s_f = SD.open(path, FILE_WRITE);
    if (!s_f) { s_mounted = false; snprintf(s_msg, sizeof(s_msg), "cannot create the file"); return false; }
    s_st = SdCapStats();
    snprintf(s_st.file, sizeof(s_st.file), "%s", path);
    s_st.start_ms = millis();
    snprintf(s_mac, sizeof(s_mac), "%s", mac);
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "");
    s_ch = ch;
    s_ble = ble;
    s_sl = 0; s_failed = false; s_have_t0 = false; s_msg[0] = 0;
    char desc[64];
    if (ble) snprintf(desc, sizeof(desc), "BLE adverts from %s", s_mac);
    else snprintf(desc, sizeof(desc), "802.11 frames to or from %s on channel %u", s_mac, ch);
    write_headers(desc);
    s_state = SDC_REC;
    s_last_flush = millis();
    Serial.printf("[cap] recording %s -> %s\n", s_mac, path);
    return true;
}

void sdcap_stop(const char* why) {
    if (s_state != SDC_REC) return;
    s_state = SDC_STOPPING;
    s_stop_ms = millis();
    snprintf(s_msg, sizeof(s_msg), "%s", why);
}

void sdcap_abort(const char* why) { if (s_state != SDC_IDLE) finish(why); }

void sdcap_event(const uint8_t* cb, size_t n) {
    if (s_state == SDC_IDLE) return;
    bool act = false, endf = false;
    uint32_t d = 0;
    (void)ss_cbor_get_bool(cb, n, "a", &act);
    (void)ss_cbor_get_bool(cb, n, "e", &endf);
    if (ss_cbor_get_uint(cb, n, "d", &d)) s_st.dropped = d;
    ss_cbor_val_t r;
    if (ss_cbor_map_get(cb, n, "r", &r) && r.type == SS_CBOR_BYTES) {
        const uint8_t* p = r.ptr;
        size_t left = r.n;
        while (left >= CAP_REC_HDR) {
            uint32_t t = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            uint16_t caplen = (uint16_t)(p[8] | (p[9] << 8)), origlen = (uint16_t)(p[10] | (p[11] << 8));
            size_t rec = CAP_REC_HDR + (size_t)caplen;
            char verdict[64];
            const char* vp = nullptr;
            if (p[7] & CAP_F_NOTE) {                                     // annotation: u8 conf, u8 best, u8 rlen, rule
                if (rec + 3 > left) break;
                const uint8_t* an = p + rec;
                size_t rl = an[2];
                if (rec + 3 + rl > left) break;
                char rule[28];
                size_t k = rl < sizeof(rule) - 1 ? rl : sizeof(rule) - 1;
                memcpy(rule, an + 3, k);
                rule[k] = 0;
                snprintf(verdict, sizeof(verdict), "Rule: %s (strongest %u, total %u)", rule, an[1], an[0]);
                vp = verdict;
                rec += 3 + rl;
            }
            if (rec > left) break;
            if (!s_failed) {
                if (p[6] == 2) { if (s_ble) write_ble_epb(t, (int8_t)p[5], p[7], p + CAP_REC_HDR, caplen, vp); }
                else if (!s_ble) write_epb(t, p[4], (int8_t)p[5], p + CAP_REC_HDR, caplen, origlen, vp);
            }
            p += rec;
            left -= rec;
        }
    }
    if (s_failed) { finish(s_msg); return; }
    if (endf) finish(s_state == SDC_STOPPING ? "saved" : "ended by the C5");
}

void sdcap_tick(uint32_t now) {
    if (s_state == SDC_IDLE) return;
    if (s_state == SDC_STOPPING && now - s_stop_ms > STOP_WAIT_MS) { finish("saved (no end from the C5)"); return; }
    if (now - s_last_flush >= 2000) {
        s_last_flush = now;
        stage_flush();
        if (s_f) s_f.flush();
        if (s_failed) finish(s_msg);
    }
}

// ---- alerts.csv: one row per alert (issue #10) -------------------------------------------------------------
// Appended from the same task that writes captures, so the two never touch the card at once. The device has no
// clock: boot_ms is milliseconds since this CYD booted. Switched on by SET > PRIVACY (off at every boot).
void sdcap_log_alert(const Det& d) {
    static const char* const TYPES[] = { "drone", "tracker", "camera", "bodycam", "attack", "unknown" };
    if (!sdcap_card_ok()) return;
    const char* path = "/signalsweep/alerts.csv";
    bool fresh = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) { s_mounted = false; return; }
    if (fresh) f.print("boot_ms,type,confidence,strongest,mac,name,ssid,radio,band,channel,rssi,times_heard\r\n");
    char name[40], ssid[40];
    for (int k = 0; k < 2; k++) {                                    // CSV-safe: no commas, quotes or line breaks
        const char* s = k == 0 ? d.name : d.ssid;
        char* o = k == 0 ? name : ssid;
        size_t j = 0;
        for (; *s && j + 1 < 39; s++) o[j++] = (*s == ',' || *s == '"' || *s == '\r' || *s == '\n') ? '_' : *s;
        o[j] = 0;
    }
    f.printf("%lu,%s,%u,%u,%s,%s,%s,%s,%u,%u,%d,%u\r\n", (unsigned long)millis(), TYPES[d.type <= 5 ? d.type : 5], d.conf, d.best,
             d.mac, name, ssid, d.radio == 1 ? "ble" : d.radio == 2 ? "both" : "wifi", d.band, d.ch, (int)d.rssi, d.seen);
    f.close();
}

// Console test hook ('a'): print alerts.csv over serial between marker lines, so it can be read without pulling the card.
void sdcap_dump_alerts(void) {
    if (!sdcap_card_ok()) { Serial.printf("[alerts] no card (%s)\n", sdcap_card_text()); return; }
    File f = SD.open("/signalsweep/alerts.csv", FILE_READ);
    if (!f) { Serial.println("[alerts] no alerts.csv yet"); return; }
    Serial.printf("[alerts] --- begin (%lu bytes) ---\n", (unsigned long)f.size());
    uint8_t buf[128];
    size_t total = 0;
    while (f.available() && total < 65536) {
        size_t n = f.read(buf, sizeof(buf));
        if (!n) break;
        Serial.write(buf, n);
        total += n;
        delay(1);                                                    // let the UART drain
    }
    f.close();
    Serial.println("\n[alerts] --- end ---");
}
