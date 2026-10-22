// SPDX-License-Identifier: GPL-3.0-or-later
// listexport.cpp — see listexport.h.
#include "listexport.h"
#include <SD.h>
#include "sdcap.h"
#include "theme.h"                    // CONF_ALERT_MIN

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

static const char* const TYPES[] = { "drone", "tracker", "camera", "bodycam", "attack", "unknown" };
static const char* const MAC_MODES[] = { "full", "oui", "none" };

// Text for a CSV cell or a JSON string: commas, quotes, backslashes and control characters become '_'.
static void clean(char* out, size_t cap, const char* s) {
    size_t j = 0;
    for (; *s && j + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        out[j++] = (c < 0x20 || c == ',' || c == '"' || c == '\\') ? '_' : (char)c;
    }
    out[j] = 0;
}

static void mac_text(char* out, size_t cap, const char* mac, ListMacMode mode) {
    if (mode == LMAC_NONE) { out[0] = 0; return; }
    if (mode == LMAC_FULL) { clean(out, cap, mac); return; }
    snprintf(out, cap, "%.8s:xx:xx:xx", mac);                            // AA:BB:CC + masked device part
}

static const char* radio_text(uint8_t r) { return r == 1 ? "ble" : r == 2 ? "both" : "wifi"; }

bool listexport_save(const Det* const* rows, int n, ListMacMode mode, const char* filter, char* msg, size_t msgsz) {
    if (n <= 0) { snprintf(msg, msgsz, "nothing to save"); return false; }
    if (!sdcap_card_ok()) { snprintf(msg, msgsz, "no SD card"); return false; }
    int k = 1;
    char csv[40], json[40];
    for (; k < 1000; k++) {
        snprintf(csv, sizeof(csv), "/signalsweep/list_%03d.csv", k);
        snprintf(json, sizeof(json), "/signalsweep/list_%03d.json", k);
        if (!SD.exists(csv) && !SD.exists(json)) break;
    }
    if (k >= 1000) { snprintf(msg, msgsz, "no free file name"); return false; }
    File fc = SD.open(csv, FILE_WRITE);
    File fj = SD.open(json, FILE_WRITE);
    if (!fc || !fj) {
        if (fc) fc.close();
        if (fj) fj.close();
        snprintf(msg, msgsz, "cannot create the files");
        return false;
    }
    uint32_t now = millis();
    char flt[64];
    clean(flt, sizeof(flt), filter ? filter : "");
    fc.print("type,confidence,strongest,mac,name,ssid,radio,band,channel,rssi,first_seen_ago_ms,last_seen_ago_ms,times_heard,alerted,muted\r\n");
    fj.printf("{\"schema\":\"signalsweep-detections\",\"version\":1,\"fw\":\"%s\",\"boot_ms\":%lu,\"mac_mode\":\"%s\",\"filter\":\"%s\",\"count\":%d,\"detections\":[",
              FW_VERSION, (unsigned long)now, MAC_MODES[mode], flt, n);
    bool ok = true;
    for (int i = 0; i < n; i++) {
        const Det& d = *rows[i];
        char mac[24], name[40], ssid[40];
        mac_text(mac, sizeof(mac), d.mac, mode);
        clean(name, sizeof(name), d.name);
        clean(ssid, sizeof(ssid), d.ssid);
        const char* type = TYPES[d.type <= 5 ? d.type : 5];
        bool alerted = d.best >= CONF_ALERT_MIN;                        // Det::alerted is never set on the CYD; the threshold is what an alert means
        unsigned long first_ago = (unsigned long)(now - d.first_ms), last_ago = (unsigned long)(now - d.last_ms);
        size_t w1 = fc.printf("%s,%u,%u,%s,%s,%s,%s,%u,%u,%d,%lu,%lu,%u,%u,%u\r\n", type, d.conf, d.best, mac, name, ssid, radio_text(d.radio),
                              d.band, d.ch, (int)d.rssi, first_ago, last_ago, d.seen, alerted ? 1 : 0, d.muted ? 1 : 0);
        size_t w2 = fj.printf("%s{\"type\":\"%s\",\"confidence\":%u,\"strongest\":%u,\"mac\":\"%s\",\"name\":\"%s\",\"ssid\":\"%s\",\"radio\":\"%s\","
                              "\"band\":%u,\"channel\":%u,\"rssi\":%d,\"first_seen_ago_ms\":%lu,\"last_seen_ago_ms\":%lu,\"times_heard\":%u,"
                              "\"alerted\":%s,\"muted\":%s}",
                              i ? "," : "", type, d.conf, d.best, mac, name, ssid, radio_text(d.radio), d.band, d.ch, (int)d.rssi,
                              first_ago, last_ago, d.seen, alerted ? "true" : "false", d.muted ? "true" : "false");
        if (!w1 || !w2) ok = false;
    }
    fj.print("]}\r\n");
    fc.close();
    fj.close();
    if (!ok) { snprintf(msg, msgsz, "SD write failed"); return false; }
    snprintf(msg, msgsz, "list_%03d: %d saved", k, n);
    Serial.printf("[list] saved %s and .json (%d detections, MAC %s, %s)\n", csv, n, MAC_MODES[mode], flt);
    return true;
}
