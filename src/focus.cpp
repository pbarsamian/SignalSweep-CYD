// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep CYD — focus mode profile collection
#include "focus.h"

FocusProfile g_focus;
static SemaphoreHandle_t s_mux = nullptr;

void focus_init() { if (!s_mux) s_mux = xSemaphoreCreateMutex(); }

void focus_start(const uint8_t mac[6], uint8_t radio, uint8_t channel) {
    if (!s_mux) focus_init();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    g_focus = FocusProfile();          // fresh profile for the new target
    memcpy(g_focus.mac, mac, 6);
    g_focus.radio = radio;
    g_focus.channel = channel;
    g_focus.started_ms = millis();
    g_focus.active = true;
    xSemaphoreGive(s_mux);
    Serial.printf("[focus] start %02X:%02X:%02X:%02X:%02X:%02X %s ch%d\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        radio == FOCUS_WIFI ? "WiFi" : "BLE", channel);
}

void focus_stop() {
    if (!g_focus.active) return;
    g_focus.active = false;
    Serial.println("[focus] stop, sweeping resumes");
}

bool focus_target(const uint8_t mac[6], uint8_t radio) {
    return g_focus.active && g_focus.radio == radio && memcmp(g_focus.mac, mac, 6) == 0;
}

bool focus_copy(FocusProfile& out) {
    if (!s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    out = g_focus;
    xSemaphoreGive(s_mux);
    return true;
}

// ── BLE ─────────────────────────────────────────────────────────────────────
void focus_note_ble(const uint8_t* p, size_t len) {
    if (xSemaphoreTake(s_mux, 0) != pdTRUE) return;
    uint32_t now = millis();
    g_focus.adv_count++;
    if (g_focus.last_adv_ms) {
        uint32_t gap = now - g_focus.last_adv_ms;
        // Scan responses follow their advert within a few ms: not an interval
        if (gap >= 15 && (g_focus.adv_int_min == 0 || gap < g_focus.adv_int_min))
            g_focus.adv_int_min = gap;
    }
    g_focus.last_adv_ms = now;
    if (p && len) {
        if (len > sizeof(g_focus.raw)) len = sizeof(g_focus.raw);
        memcpy(g_focus.raw, p, len);
        g_focus.raw_len = (uint8_t)len;
    }
    xSemaphoreGive(s_mux);
}

// ── WiFi ────────────────────────────────────────────────────────────────────
static void copy_printable(char* dst, size_t n, const uint8_t* s, int len) {
    int j = 0;
    for (int i = 0; i < len && j < (int)n - 1; i++)
        if (s[i] >= 0x20 && s[i] < 0x7F) dst[j++] = (char)s[i];
    dst[j] = 0;
}

// WPS (Wi-Fi Simple Config) attributes: type(2, BE) len(2, BE) value
static void parse_wps(const uint8_t* a, int len) {
    int p = 0;
    while (p + 4 <= len) {
        uint16_t t = (a[p] << 8) | a[p+1];
        uint16_t l = (a[p+2] << 8) | a[p+3];
        const uint8_t* v = a + p + 4;
        if (p + 4 + l > len) break;
        switch (t) {
            case 0x1021: copy_printable(g_focus.wps_manuf,     33, v, l); break;
            case 0x1023: copy_printable(g_focus.wps_model,     33, v, l); break;
            case 0x1024: copy_printable(g_focus.wps_model_num, 33, v, l); break;
            case 0x1042: copy_printable(g_focus.wps_serial,    33, v, l); break;
            case 0x1011: copy_printable(g_focus.wps_devname,   33, v, l); break;
            case 0x1054: if (l >= 2) g_focus.wps_cat = (v[0] << 8) | v[1]; break;
            case 0x1044: if (l >= 1) g_focus.wps_state = v[0]; break;
        }
        p += 4 + l;
    }
    g_focus.has_wps = true;
}

void focus_note_wifi(uint8_t subtype, const uint8_t* f, int length) {
    if (xSemaphoreTake(s_mux, 0) != pdTRUE) return;
    g_focus.frames++;
    if (subtype == 8) g_focus.beacons++;
    else if (subtype == 5) g_focus.probe_resps++;
    else if (subtype == 4) g_focus.probe_reqs++;
    else g_focus.other_mgmt++;

    if (subtype == 4 || subtype == 5 || subtype == 8) {
        int off = (subtype == 4) ? 24 : 36;
        int blen = length - off - 4;   // -4 FCS
        const uint8_t* b = f + off;
        int i = 0;
        while (blen > 0 && i + 2 <= blen) {
            uint8_t id = b[i], el = b[i+1];
            if (i + 2 + el > blen) break;
            const uint8_t* e = b + i + 2;
            g_focus.ie_map[id >> 3] |= (1 << (id & 7));
            if (id == 221 && el >= 4) {
                bool have = false;
                for (int k = 0; k < g_focus.vie_count; k++)
                    if (memcmp(g_focus.vie[k], e, 4) == 0) have = true;
                if (!have && g_focus.vie_count < FOCUS_MAX_VIE)
                    memcpy(g_focus.vie[g_focus.vie_count++], e, 4);
                if (e[0]==0x00 && e[1]==0x50 && e[2]==0xF2 && e[3]==0x04)
                    parse_wps(e + 4, el - 4);
            }
            if (id == 45 && el >= 7) {
                // HT capabilities: RX MCS bitmask bytes 3..6 = spatial streams 1..4
                uint8_t n = 0;
                for (int k = 3; k <= 6; k++) if (e[k]) n++;
                if (n > g_focus.ht_streams) g_focus.ht_streams = n;
            }
            i += 2 + el;
        }
    }
    xSemaphoreGive(s_mux);
}
