// SPDX-License-Identifier: GPL-3.0-or-later
// ui.cpp — header, tab bar and the SCOPE / LIST / INFO / SET pages (direct TFT rendering, no
// sprites). Ported in look and layout from the standalone firmware's ui.cpp; the data now comes
// from the C5 via model.cpp. Pages draw in place (no full clears per frame) to avoid flicker.
#include "ui.h"
#include <Preferences.h>
#include "board_pins.h"
#include "theme.h"
#include "model.h"
#include "mode.h"
#include "vendor.h"
#include "about_credits.h"     // generated from CREDITS.md by about_pre.py
#include <stdarg.h>

extern TFT_eSPI tft;                 // owned by status_ui.cpp
extern uint8_t  g_backlight;
void status_ui_reset();              // status_ui.cpp: repaint the diagnostics page from scratch

UiSettings g_ui_set;
// mode 0 = both radios run together (bpct = BLE scan window as a percent of its interval); mode 1 = sliced
const SchedPreset SCHED_PRESETS[] = {
    { "WI-FI",     0,  20, 4000, 2000, 6000 },
    { "BALANCED",  0,  50, 4000, 2000, 6000 },     // BLE window as before the scheduler; Wi-Fi now dwells 6 s per band
    { "BLE",       0,  80, 4000, 2000, 6000 },
    { "SLICED",    1,  50, 4000, 2000, 6000 },
};
const int SCHED_N = (int)(sizeof(SCHED_PRESETS) / sizeof(SCHED_PRESETS[0]));
static UiCallbacks s_cb = {};
static Preferences s_prefs;

// ---- state --------------------------------------------------------------------------
static int      s_tab = TAB_SCOPE;
static bool     s_dirty = true;          // the page needs a full repaint
static bool     s_hdr_dirty = true;
static bool     s_diag = false;          // SET > DEVICE > DIAGNOSTICS overlay
static int      s_set_page = 0;          // 0 alerts, 1 device, 2 profile (open-network), 3 about
#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif
static int      s_scroll = 0;            // LIST / INFO scroll offset
static int      s_sub = 0;               // INFO sub-page: 0 INFO, 1 LINKS, 2 TRAFF, 3 RAW, 4 HUNT
#define SC_AUTO 3                         // scope follows what the C5 is scanning
static int      s_scope_pref = SC_AUTO;  // what the band button selects: 2, 5, 0 or SC_AUTO
static int      s_scope_band = 2;        // what is shown now: 2 = 2.4 GHz, 5 = 5 GHz, 0 = BLE
static uint8_t  s_scan = 3, s_lock = 0;  // the C5's radio state (1 Wi-Fi, 2 BLE, 3 both) and focus lock
static uint32_t s_drawn_ms = 0;
static char     s_sel_mac[18] = {};
static bool     s_have_sel = false;
static char     s_toast[56] = {};
static uint32_t s_toast_until = 0;
static uint32_t s_beep_until = 0, s_led_until = 0;

int  ui_active_tab() { return s_tab; }
bool ui_wants_aps() { return s_tab == TAB_SCOPE && !s_diag && s_scope_band != 0; }
bool ui_wants_profile(char* mac_out) {
    if (s_tab != TAB_INFO || !s_have_sel || s_diag) return false;
    strncpy(mac_out, s_sel_mac, 18);
    mac_out[17] = 0;
    return true;
}
bool ui_wants_ble() { return s_tab == TAB_SCOPE && !s_diag && s_scope_band == 0; }
void ui_invalidate() { s_dirty = true; s_hdr_dirty = true; }

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void toast(const char* msg) {
    strncpy(s_toast, msg, sizeof(s_toast) - 1);
    s_toast[sizeof(s_toast) - 1] = 0;
    s_toast_until = millis() + 2500;
}

static void fmt_ago(char* out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000;
    if (s < 60)        snprintf(out, n, "%us ago", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um %us ago", (unsigned)(s / 60), (unsigned)(s % 60));
    else               snprintf(out, n, "%uh %um ago", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

// ---- settings persistence --------------------------------------------------------------
static void settings_load() {
    s_prefs.begin("ssui", true);
    for (int i = 0; i < 5; i++) { char k[4]; snprintf(k, sizeof(k), "d%d", i); g_ui_set.detect[i] = s_prefs.getBool(k, true); }
    g_ui_set.speaker = s_prefs.getBool("spk", true);
    g_ui_set.led = s_prefs.getBool("led", true);
    g_ui_set.brightness = s_prefs.getUChar("bri", 200);
    g_ui_set.sched = s_prefs.getUChar("sch", 1);
    if (g_ui_set.sched >= SCHED_N) g_ui_set.sched = 1;
    s_prefs.end();
    if (g_ui_set.brightness < 25) g_ui_set.brightness = 25;
}

static void settings_save() {
    s_prefs.begin("ssui", false);
    for (int i = 0; i < 5; i++) { char k[4]; snprintf(k, sizeof(k), "d%d", i); s_prefs.putBool(k, g_ui_set.detect[i]); }
    s_prefs.putBool("spk", g_ui_set.speaker);
    s_prefs.putBool("led", g_ui_set.led);
    s_prefs.putUChar("bri", g_ui_set.brightness);
    s_prefs.putUChar("sch", g_ui_set.sched);
    s_prefs.end();
}

void ui_init(const UiCallbacks& cb) {
    s_cb = cb;
    settings_load();
    g_backlight = g_ui_set.brightness;
    ledcWrite(0, g_backlight);
    // Speaker on LEDC channel 1 (SC8002B amp), RGB LED active-low and off
    ledcSetup(1, 2000, 8);
    ledcAttachPin(PIN_SPEAKER, 1);
    ledcWrite(1, 0);
    pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
    digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
    s_dirty = s_hdr_dirty = true;
}

void ui_alert(uint8_t type) {
    if (type > T_ATTACK || !g_ui_set.detect[type]) return;
    if (g_ui_set.speaker) {
        static const uint16_t F[] = { TONE_DRONE, TONE_TRACKER, TONE_CAMERA, TONE_BODYCAM, TONE_ATTACK };
        ledcWriteTone(1, F[type]);
        s_beep_until = millis() + TONE_MS;
    }
    if (g_ui_set.led) {
        uint8_t r = HIGH, g = HIGH, b = HIGH;
        switch (type) {
            case T_DRONE:   b = LOW; break;
            case T_TRACKER: r = LOW; break;
            case T_CAMERA:  r = LOW; break;
            case T_BODYCAM: r = LOW; b = LOW; break;
            case T_ATTACK:  r = LOW; g = LOW; break;
        }
        digitalWrite(PIN_LED_R, r); digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_B, b);
        s_led_until = millis() + 200;
    }
}

static void alert_tick(uint32_t now) {
    if (s_beep_until && (int32_t)(now - s_beep_until) >= 0) { ledcWriteTone(1, 0); ledcWrite(1, 0); s_beep_until = 0; }
    if (s_led_until && (int32_t)(now - s_led_until) >= 0) {
        digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
        s_led_until = 0;
    }
}

// ---- header ------------------------------------------------------------------------------
static void draw_header(const UiCtx& c) {
    int alerts = model_alert_count();
    uint16_t bat = c.c5->link_up ? c.c5->batt_mv : 0;
    static char last[48] = {};
    char key[48];
    bool ble_fresh = g_model.ble_ms && (millis() - g_model.ble_ms) < 15000;
    int ble_n = ble_fresh ? (int)g_model.ble_total : -1;
    snprintf(key, sizeof(key), "%d|%u|%d|%d|%d|%d", c.c5->link_up, bat, (int)c.mode, alerts, c.c5->batt_low, ble_n);
    if (!s_hdr_dirty && strcmp(key, last) == 0) return;
    strncpy(last, key, sizeof(last) - 1);
    s_hdr_dirty = false;

    tft.fillRect(0, 0, SCREEN_W, HDR_H, C_BLACK);
    tft.drawFastHLine(0, HDR_H - 1, SCREEN_W, C_ACCENT);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_ACCENT, C_BLACK);
    tft.drawString("SIGNALSWEEP", 4, HDR_H / 2, 1);

    // Persistent RF-mode pill (reads the one authoritative mode value)
    uint16_t pc = (c.mode == SS_MODE_SILENT_RX) ? C_SIGNAL : C_WARN;
    tft.fillRoundRect(76, 2, 62, HDR_H - 5, 5, pc);
    tft.setTextColor(C_BLACK, pc);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(c.mode == SS_MODE_SILENT_RX ? "RX-ONLY" : "TX-DET", 107, HDR_H / 2, 1);

    // Link dot + battery
    tft.fillCircle(150, HDR_H / 2, 3, c.c5->link_up ? C_SIGNAL : C_BAD);
    tft.setTextDatum(ML_DATUM);
    if (bat) {
        char b[12];
        snprintf(b, sizeof(b), "%u.%02uV", bat / 1000, (bat % 1000) / 10);
        uint16_t col = bat >= 3700 ? C_SIGNAL : bat >= 3450 ? C_WARN : C_BAD;
        tft.setTextColor(col, C_BLACK);
        tft.drawString(b, 158, HDR_H / 2, 1);
    } else {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.drawString(c.c5->link_up ? "n/a" : "no C5", 158, HDR_H / 2, 1);
    }

    // BLE devices heard recently (dim until the first reply, or if it goes stale)
    char bb[12];
    if (ble_n >= 0) snprintf(bb, sizeof(bb), "BLE %d", ble_n); else snprintf(bb, sizeof(bb), "BLE -");
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ble_n > 0 ? C_ACCENT : C_MDGREY, C_BLACK);
    tft.drawString(bb, 204, HDR_H / 2, 1);

    // Alert badge
    char buf[16];
    snprintf(buf, sizeof(buf), "%d ALERTS", alerts);
    uint16_t bc = alerts > 0 ? C_CAMERA : C_DKGREY;
    tft.fillRect(SCREEN_W - 68, 2, 66, HDR_H - 4, bc);
    tft.fillCircle(SCREEN_W - 68, HDR_H / 2, (HDR_H - 4) / 2, bc);
    tft.fillCircle(SCREEN_W - 2, HDR_H / 2, (HDR_H - 4) / 2, bc);
    tft.setTextColor(C_WHITE, bc);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(buf, SCREEN_W - 35, HDR_H / 2, 1);
}

// ---- tab bar -----------------------------------------------------------------------------
static const char* TAB_NAMES[] = { "SCOPE", "LIST", "INFO", "SET" };

static void draw_tabs() {
    tft.fillRect(0, TAB_Y, SCREEN_W, TAB_H, C_BLACK);
    tft.drawFastHLine(0, TAB_Y, SCREEN_W, C_BORDER);
    for (int i = 0; i < 4; i++) {
        int x = i * TAB_W;
        bool act = (s_tab == i);
        if (act) {
            tft.fillRect(x, TAB_Y, TAB_W, TAB_H, 0x0082);
            tft.drawFastHLine(x, TAB_Y, TAB_W, C_ACCENT);
            tft.drawFastHLine(x, TAB_Y + 1, TAB_W, 0x0488);
        }
        if (i > 0) tft.drawFastVLine(x, TAB_Y + 4, TAB_H - 8, C_BORDER);
        tft.setTextColor(act ? C_ACCENT : C_LTGREY, act ? 0x0082 : C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(TAB_NAMES[i], x + TAB_W / 2, TAB_Y + TAB_H / 2, 1);
    }
}

static void clear_page() { tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK); }

// ---- detection rows (shared by SCOPE and LIST) ----------------------------------------------
static int visible_index(int nth) {          // the nth visible detection's slot, or -1
    int seen = 0;
    for (int i = 0; i < g_model.ndets; i++) {
        if (!g_model.dets[i].used || g_model.dets[i].type == T_UNKNOWN) continue;
        if (seen == nth) return i;
        seen++;
    }
    return -1;
}

static int rssi_bars(int rssi) { return rssi >= -60 ? 5 : rssi >= -70 ? 4 : rssi >= -80 ? 3 : rssi >= -90 ? 2 : 1; }

// ---- SCOPE -----------------------------------------------------------------------------------
static const uint8_t CH5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                               132, 136, 140, 144, 149, 153, 157, 161, 165 };
#define N5 ((int)sizeof(CH5))

// Bluetooth SIG company name: the generated database first, then the few names our own rules use
// (so they still show if the database is the small fallback).
const char* company_name_ui(uint16_t id);
static const char* company_name(uint16_t id) {
    if (const char* v = vendor_for_company(id)) return v;
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x0D53: return "Luxottica";
        case 0x03C2: return "Snap";
        case 0x0E29: return "Flipper";
    }
    return nullptr;
}

const char* company_name_ui(uint16_t id) { return company_name(id); }

// IEEE vendor for a "AA:BB:.." MAC. Locally administered (randomised) addresses carry no vendor.
static const char* mac_vendor(const char* mac) {
    unsigned b[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return nullptr;
    if (b[0] & 0x02) return nullptr;
    uint8_t m[6];
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)b[i];
    return vendor_for_mac(m);
}

template <class G>
static void draw_ble_canvas(G& g, int sx, int sy, int sw, int sh) {
    g.fillRect(sx, sy, sw, sh, C_BLACK);
    g.drawRect(sx, sy, sw, sh, C_BORDER);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(C_MDGREY, C_BLACK);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "BLE (passive): %d heard", (int)g_model.ble_total);
    g.drawString(hdr, sx + 4, sy + 8, 1);
    int rows = (sh - 18) / 11;                       // 8 px font + 3 px gap
    for (int i = 0; i < rows && i < g_model.nble; i++) {
        const BleRow& r = g_model.ble[i];
        int y = sy + 18 + i * 11;
        int w = (int)(((r.rssi + 100) / 60.0f) * 60.0f); w = w < 2 ? 2 : w > 60 ? 60 : w;
        g.fillRect(sx + 3, y, w, 8, C_SIGNAL);
        g.fillRect(sx + 3 + w, y, 60 - w, 8, C_PANEL);
        char label[40];
        const char* who = r.name[0] ? r.name : (r.has_company ? company_name(r.company) : nullptr);
        if (who) snprintf(label, sizeof(label), "%s", who);
        else if (r.has_company) snprintf(label, sizeof(label), "co 0x%04X", r.company);
        else snprintf(label, sizeof(label), "%s", r.mac + 9);          // last 3 octets
        g.setTextColor(C_WHITE, C_BLACK);
        g.drawString(label, sx + 68, y + 4, 1);
        char rb[8]; snprintf(rb, sizeof(rb), "%d", r.rssi);
        g.setTextDatum(MR_DATUM);
        g.setTextColor(C_LTGREY, C_BLACK);
        g.drawString(rb, sx + sw - 4, y + 4, 1);
        g.setTextDatum(ML_DATUM);
    }
    if (g_model.nble == 0) {
        g.setTextColor(C_MDGREY, C_BLACK);
        g.setTextDatum(MC_DATUM);
        g.drawString("listening... (names need a scan response; passive scan can't ask)", sx + sw / 2, sy + sh / 2 + 6, 1);
    }
}

// Access points of one band, strongest first (indices into g_model.aps)
static int ap_order(int band, int* order) {
    int n = 0;
    for (int i = 0; i < g_model.naps; i++)
        if (g_model.aps[i].used && g_model.aps[i].band == band) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && g_model.aps[order[j]].rssi < g_model.aps[k].rssi) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    return n;
}

// Wi-Fi band views list the strongest access points under the scope. Tap one to open its INFO page and
// focus it (LINKS / TRAFF / RAW / HUNT then work on it like on any detection).
static void draw_ap_rows(int sy, int sh) {
    int order[MODEL_MAX_APS];
    int n = ap_order(s_scope_band, order);
    int ry = sy + sh + 32, shown = 0;
    for (int k = 0; k < n && ry + ROW_H <= PAGE_Y + PAGE_H; k++) {
        const ApRow& a = g_model.aps[order[k]];
        uint16_t bg = (shown % 2 == 0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, ry, SCREEN_W, ROW_H - 1, bg);
        tft.fillRect(0, ry, 3, ROW_H - 1, C_ACCENT);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_ACCENT, bg);
        tft.drawString("AP", 6, ry + ROW_H / 2, 1);
        tft.setTextColor(a.ssid[0] ? C_WHITE : C_MDGREY, bg);
        char nm[24];
        snprintf(nm, sizeof(nm), "%.22s", a.ssid[0] ? a.ssid : "(hidden)");
        tft.drawString(nm, 26, ry + ROW_H / 2, 1);
        char rb[16];
        snprintf(rb, sizeof(rb), "ch%d  %d", a.ch, a.rssi);
        tft.setTextColor(C_LTGREY, bg);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(rb, SCREEN_W - 16, ry + ROW_H / 2, 1);
        int cx = SCREEN_W - 8, cy = ry + ROW_H / 2;
        tft.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 3, cy, C_ACCENT);
        ry += ROW_H; shown++;
    }
    if (ry < PAGE_Y + PAGE_H) tft.fillRect(0, ry, SCREEN_W, PAGE_Y + PAGE_H - ry, C_BLACK);
    if (shown == 0) {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No access points yet", SCREEN_W / 2, sy + sh + 46, 2);
    }
}

// The scope canvas is drawn off-screen and pushed to the panel in one go. Drawing it straight onto the panel
// (clear, then redraw) made the whole scope, and the band pill most of all, flash on every refresh.
// Falls back to direct drawing if the 48 KB sprite cannot be allocated.
static TFT_eSprite s_cv(&tft);
static bool s_cv_ok = false, s_cv_tried = false;

// Everything inside the canvas border: grid, bars or BLE list, the band pill. (sx, sy) is the canvas
// origin in the target's own coordinates (0,0 for the sprite, the real position for the panel).
template <class G>
static void scope_canvas(G& g, int sx, int sy, int sw, int sh) {
    g.fillRect(sx, sy, sw, sh, C_BLACK);
    g.drawRect(sx, sy, sw, sh, C_BORDER);

    // Strength: -100 dBm -> 0, -40 dBm -> 80 (the standalone scope's mapping)
    auto strength = [](int rssi) { int v = (int)(((rssi + 100) / 60.0f) * 80.0f); return v < 0 ? 0 : v > 80 ? 80 : v; };

    if (s_scope_band == 0) {
        draw_ble_canvas(g, sx, sy, sw, sh);
    } else if (s_scope_band == 2) {
        for (int i = 1; i < 4; i++) g.drawFastHLine(sx + 1, sy + (i * sh) / 4, sw - 2, C_GRID);
        static uint8_t pow24[320];
        memset(pow24, 0, sizeof(pow24));
        for (int ch = 1; ch <= 11; ch++) {                       // channel guide lines
            int xp = sx + ((ch - 1) * (sw - 1)) / 10;
            g.drawFastVLine(xp, sy + 1, sh - 2, C_GRID);
        }
        for (int i = 0; i < g_model.naps; i++) {                 // a tapered hump per AP
            const ApRow& a = g_model.aps[i];
            if (a.band != 2 || a.ch < 1 || a.ch > 11) continue;
            int x = ((a.ch - 1) * 319) / 10, str = strength(a.rssi);
            for (int dx = -9; dx <= 9; dx++) {
                int xi = x + dx;
                if (xi < 0 || xi >= 320) continue;
                int v = pow24[xi] + str / (abs(dx) + 1);
                pow24[xi] = (uint8_t)(v > 100 ? 100 : v);
            }
        }
        for (int x = 0; x < sw; x++) {                           // bright top half, dim below
            uint8_t v = pow24[(x * 319) / (sw - 1)];
            int h = (v * (sh - 4)) / 100;
            if (h > 0) {
                int top_h = h / 2 + 1, bot_h = h - top_h, top_y = sy + sh - 2 - h;
                g.drawFastVLine(sx + x, top_y, top_h, C_SIGNAL);
                if (bot_h > 0) g.drawFastVLine(sx + x, top_y + top_h, bot_h, C_SIGNAL2);
            }
        }
    } else {
        for (int i = 1; i < 4; i++) g.drawFastHLine(sx + 1, sy + (i * sh) / 4, sw - 2, C_GRID);
        int slot = sw / N5;                                      // 25 five-GHz channels, one bar each
        static uint8_t pow5[N5];
        memset(pow5, 0, sizeof(pow5));
        for (int i = 0; i < g_model.naps; i++) {
            const ApRow& a = g_model.aps[i];
            if (a.band != 5) continue;
            for (int k = 0; k < N5; k++)
                if (CH5[k] == a.ch) { int s = strength(a.rssi); if (s > pow5[k]) pow5[k] = (uint8_t)s; }
        }
        for (int k = 0; k < N5; k++) {
            int x = sx + 1 + k * slot;
            g.drawFastVLine(x - 1, sy + 1, sh - 2, C_GRID);
            int h = (pow5[k] * (sh - 4)) / 80;
            if (h > 0) {
                int top_h = h / 2 + 1, bot_h = h - top_h, top_y = sy + sh - 2 - h;
                g.fillRect(x + 1, top_y, slot - 3, top_h, C_SIGNAL);
                if (bot_h > 0) g.fillRect(x + 1, top_y + top_h, slot - 3, bot_h, C_SIGNAL2);
            }
        }
    }

    // Band selector: tap to switch (inside the canvas, top right)
    g.fillRect(sx + sw - 58, sy + 2, 56, 13, C_PANEL);
    g.drawRect(sx + sw - 58, sy + 2, 56, 13, C_ACCENT);
    g.setTextColor(C_ACCENT, C_PANEL);
    g.setTextDatum(MC_DATUM);
    g.drawString(s_scope_pref == SC_AUTO ? "AUTO" : s_scope_band == 2 ? "2.4 GHz" : s_scope_band == 5 ? "5 GHz" : "BLE", sx + sw - 30, sy + 9, 1);
    if (s_scope_band != 0 && g_model.naps == 0) {
        g.setTextColor(C_MDGREY, C_BLACK);
        g.drawString("waiting for C5 scan data...", sx + sw / 2 - 20, sy + sh / 2, 1);
    }
}

static void draw_scope() {
    const int LMARGIN = 24, RMARGIN = 6;
    int sy = PAGE_Y + 1, sh = 84, sx = LMARGIN, sw = SCREEN_W - LMARGIN - RMARGIN;

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_WHITE, C_BLACK);
    const char* db[] = { "-40", "-55", "-70", "-85", "-100" };
    for (int i = 0; i < 5; i++) tft.drawString(db[i], LMARGIN - 2, sy + (i * (sh - 1)) / 4, 1);

    if (!s_cv_tried) {
        s_cv_tried = true;
        s_cv.setColorDepth(16);
        s_cv_ok = s_cv.createSprite(sw, sh) != nullptr;
        Serial.printf("[ui] scope sprite %s (free heap %u)\n", s_cv_ok ? "ok" : "FAILED, drawing direct", (unsigned)ESP.getFreeHeap());
    }
    if (s_cv_ok) { scope_canvas(s_cv, 0, 0, sw, sh); s_cv.pushSprite(sx, sy); }
    else scope_canvas(tft, sx, sy, sw, sh);

    // Axis labels under the canvas (outside it, so drawn straight to the panel in place)
    int ya = sy + sh + 1;
    tft.setTextDatum(ML_DATUM);
    if (s_scope_band == 0) {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.drawString("Ch", 0, ya + 3, 1);
    } else if (s_scope_band == 2) {
        tft.setTextColor(C_LTGREY, C_BLACK);
        tft.drawString("Ch", 0, ya + 3, 1);
        tft.setTextDatum(TC_DATUM);
        for (int ch = 1; ch <= 11; ch++) {
            int xp = sx + ((ch - 1) * (sw - 1)) / 10;
            tft.drawFastVLine(xp, sy + sh, 4, C_LTGREY);
            char cb[4]; snprintf(cb, sizeof(cb), "%d", ch);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.drawString(cb, xp, ya + 4, 1);
        }
    } else {
        int slot = sw / N5;
        tft.setTextColor(C_LTGREY, C_BLACK);
        tft.drawString("Ch", 0, ya + 3, 1);
        tft.setTextDatum(TC_DATUM);
        for (int k = 0; k < N5; k += 3) {
            int x = sx + 1 + k * slot + slot / 2;
            tft.drawFastVLine(x, sy + sh, 4, C_LTGREY);
            char cb[5]; snprintf(cb, sizeof(cb), "%d", CH5[k]);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.drawString(cb, x, ya + 4, 1);
        }
    }
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.setTextDatum(MC_DATUM);
    {                                                         // caption: what is shown | what the C5 is doing
        char how[24];
        if (s_lock == 1) snprintf(how, sizeof(how), "FOCUS Wi-Fi");
        else if (s_lock == 2) snprintf(how, sizeof(how), "FOCUS BLE");
        else if (SCHED_PRESETS[g_ui_set.sched < SCHED_N ? g_ui_set.sched : 1].mode == 1)
            snprintf(how, sizeof(how), "SLICED: %s slot", s_scan == 2 ? "BLE" : "Wi-Fi");
        else snprintf(how, sizeof(how), "%s", SCHED_PRESETS[g_ui_set.sched < SCHED_N ? g_ui_set.sched : 1].name);
        char cap[56];
        snprintf(cap, sizeof(cap), "%s  |  %s", s_scope_band == 2 ? "2.4 GHz channels" : s_scope_band == 5 ? "5 GHz channels" : "BLE devices", how);
        tft.fillRect(0, sy + sh + 17, SCREEN_W, 12, C_BLACK);
        tft.drawString(cap, sx + sw / 2, sy + sh + 22, 1);
    }

    if (s_scope_band != 0) { draw_ap_rows(sy, sh); return; }

    // Detection rows under the scope
    int ry = sy + sh + 32, shown = 0;
    for (int n = 0; ry + ROW_H <= PAGE_Y + PAGE_H; n++) {
        int i = visible_index(n);
        if (i < 0) break;
        const Det& d = g_model.dets[i];
        uint16_t tc = TCOL[d.type], bg = (shown % 2 == 0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, ry, SCREEN_W, ROW_H - 1, bg);
        tft.fillRect(0, ry, 3, ROW_H - 1, tc);
        tft.setTextColor(tc, bg);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(TSHRT[d.type], 6, ry + ROW_H / 2, 1);
        tft.setTextColor(C_WHITE, bg);
        tft.drawString(d.name, 30, ry + ROW_H / 2, 1);
        char rb[12]; snprintf(rb, sizeof(rb), "%d", d.rssi);
        tft.setTextColor(tc, bg);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(rb, SCREEN_W - 2, ry + ROW_H / 2, 1);
        ry += ROW_H; shown++;
    }
    if (ry < PAGE_Y + PAGE_H) tft.fillRect(0, ry, SCREEN_W, PAGE_Y + PAGE_H - ry, C_BLACK);
    if (shown == 0) {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No detections", SCREEN_W / 2, sy + sh + 46, 2);
    }
}

// ---- LIST ------------------------------------------------------------------------------------
static void draw_scroll_strip14(int total) {
    tft.fillRect(SCREEN_W - 14, PAGE_Y, 14, PAGE_H, C_BLACK);
    tft.drawFastVLine(SCREEN_W - 14, PAGE_Y, PAGE_H, C_BORDER);
    uint16_t uc = (s_scroll > 0) ? C_ACCENT : C_BORDER;
    tft.fillTriangle(SCREEN_W - 7, PAGE_Y + 8, SCREEN_W - 13, PAGE_Y + 20, SCREEN_W - 1, PAGE_Y + 20, uc);
    uint16_t dc = (s_scroll + ROWS_VIS < total) ? C_ACCENT : C_BORDER;
    tft.fillTriangle(SCREEN_W - 7, TAB_Y - 22, SCREEN_W - 13, TAB_Y - 34, SCREEN_W - 1, TAB_Y - 34, dc);
    if (total > ROWS_VIS) {
        int track_h = PAGE_H - 48;
        int thumb_h = max(12, track_h * ROWS_VIS / total);
        int thumb_y = PAGE_Y + 24 + (s_scroll * (track_h - thumb_h)) / max(1, total - ROWS_VIS);
        tft.fillRect(SCREEN_W - 12, thumb_y, 10, thumb_h, C_DKGREY);
    }
}

static void draw_list() {
    int total = model_visible_count();
    int maxs = max(0, total - ROWS_VIS);
    if (s_scroll > maxs) s_scroll = maxs;
    if (total == 0) {
        clear_page();
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No signals detected", SCREEN_W / 2, PAGE_Y + PAGE_H / 2, 2);
        return;
    }
    int ry = PAGE_Y, drawn = 0;
    uint32_t now = millis();
    for (int n = s_scroll; drawn < ROWS_VIS; n++) {
        int i = visible_index(n);
        if (i < 0) break;
        const Det& d = g_model.dets[i];
        uint16_t tc = TCOL[d.type], bg = (drawn % 2 == 0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, ry, SCREEN_W - 14, ROW_H, bg);
        tft.fillRect(0, ry, 3, ROW_H, tc);
        tft.setTextColor(tc, bg);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(TSHRT[d.type], 6, ry + ROW_H / 2, 1);
        tft.setTextColor(d.muted ? C_MDGREY : C_WHITE, bg);
        tft.drawString(d.name, 30, ry + ROW_H / 2, 1);
        int bars = rssi_bars(d.rssi);
        for (int b = 0; b < 5; b++) {
            int bh = 3 + b * 2;
            tft.fillRect(234 + b * 8, ry + ROW_H - 2 - bh, 6, bh, b < bars ? tc : C_BORDER);
        }
        uint32_t age = (now - d.last_ms) / 1000;
        char tb[10];
        if (age < 60) snprintf(tb, sizeof(tb), "%lus", (unsigned long)age);
        else snprintf(tb, sizeof(tb), "%lum", (unsigned long)(age / 60));
        tft.setTextColor(C_MDGREY, bg);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(tb, 230, ry + ROW_H / 2, 1);
        ry += ROW_H; drawn++;
    }
    if (ry < TAB_Y) tft.fillRect(0, ry, SCREEN_W - 14, TAB_Y - ry, C_BLACK);
    draw_scroll_strip14(total);
}

// ---- INFO (detail) ---------------------------------------------------------------------------
#define SUBTAB_Y (PAGE_Y + 26)
#define SUBTAB_H 18
#define DHEAD_Y  (SUBTAB_Y + SUBTAB_H)
#define DROW_H   18
#define DLIST_Y  (DHEAD_Y + 2)
#define DLIST_H  (TAB_Y - DLIST_Y - 2)
#define DVIS     (DLIST_H / DROW_H)
#define DVAL_X   92
#define DSTRIP_W 16

struct DRow { char k[14]; char v[44]; };
static DRow s_rows[40];
static int  s_nrows = 0;
static Det  s_sel_last;
static bool s_sel_gone = false;

#define ROW(key, ...) do { if (s_nrows < 40) { strncpy(s_rows[s_nrows].k, key, 13); s_rows[s_nrows].k[13] = 0; \
                           snprintf(s_rows[s_nrows].v, 44, __VA_ARGS__); s_nrows++; } } while (0)

static const char* addr_type_str(uint8_t t) {
    switch (t) {
        case AT_BLE_PUBLIC:        return "BLE public (IEEE)";
        case AT_BLE_RANDOM_STATIC: return "BLE random static";
        case AT_BLE_RPA:           return "BLE private (rotating)";
        case AT_BLE_NRPA:          return "BLE random (non-resolvable)";
        case AT_WIFI_UNIVERSAL:    return "WiFi universal (IEEE)";
        case AT_WIFI_LOCAL:        return "WiFi local / randomized";
        default:                   return "Unknown";
    }
}

static const char* wifi_sec_str(uint8_t s) {
    switch (s) {
        case SEC_OPEN:      return "Open (no encryption)";
        case SEC_WEP:       return "WEP";
        case SEC_WPA:       return "WPA (TKIP)";
        case SEC_WPA2:      return "WPA2-Personal";
        case SEC_WPA2_WPA3: return "WPA2/WPA3 transition";
        case SEC_WPA3:      return "WPA3-Personal (SAE)";
        case SEC_WPA2_ENT:  return "WPA2-Enterprise";
        case SEC_WPA3_ENT:  return "WPA3-Enterprise 192";
        case SEC_OWE:       return "Enhanced Open (OWE)";
        default:            return nullptr;
    }
}

static const char* ua_type_str(uint8_t t) {              // ASTM F3411 UA types
    static const char* const T[] = { "Not declared", "Aeroplane", "Helicopter / multirotor", "Gyroplane",
        "Hybrid lift (VTOL)", "Ornithopter", "Glider", "Kite", "Free balloon", "Captive balloon", "Airship",
        "Free fall / parachute", "Rocket", "Tethered powered aircraft", "Ground obstacle", "Other" };
    return t < 16 ? T[t] : "Unknown";
}

extern const char* company_name_ui(uint16_t id);

// Rows from GET_DEVICE_PROFILE: only when the profile belongs to the device on screen
static void add_profile_rows(const Det& d) {
    const Profile& p = g_profile;
    if (!p.have || strcmp(p.mac, d.mac) != 0) { ROW("Profile", "loading..."); return; }
    char t[44];
    ROW("Address", "%s", addr_type_str(p.atype));
    if (p.rmin || p.rmax) ROW("RSSI range", "%d to %d dBm", p.rmin, p.rmax);
    if (p.corr) ROW("Corroborated", "same MAC on BLE and Wi-Fi");
    if (p.dev_name[0]) ROW("Device name", "%s", p.dev_name);
    if (p.company >= 0) {
        const char* c = company_name_ui((uint16_t)p.company);
        if (c) ROW("BT company", "%s (0x%04X)", c, (unsigned)p.company); else ROW("BT company", "0x%04X", (unsigned)p.company);
    }
    if (p.has_txp) ROW("TX power", "%d dBm", p.txp);
    if (p.has_appear) ROW("Appearance", "0x%04X", p.appear);
    if (p.has_apple) ROW("Apple", "Continuity type 0x%02X", p.apple);
    for (int i = 0; i < p.nsvc; i++) {
        const char* sn = name_for_uuid16(p.svc[i]);
        if (sn) ROW("Service", "%s (0x%04X)", sn, p.svc[i]); else ROW("Service", "0x%04X", p.svc[i]);
    }
    if (p.hidden) ROW("SSID", "(hidden network)");
    if (p.probe[0]) ROW("Probing for", "%s", p.probe);
    if (p.wifi) {
        const char* sec = wifi_sec_str(p.wsec);
        if (sec) ROW("Security", "%s", sec);
        if (p.wstd) {
            const char* st = (p.wstd & WSTD_BE) ? "802.11be (WiFi 7)" : (p.wstd & WSTD_AX) ? "802.11ax (WiFi 6)" :
                             (p.wstd & WSTD_AC) ? "802.11ac (WiFi 5)" : "802.11n (WiFi 4)";
            ROW("Standard", "%s", st);
        }
        if (p.country[0]) ROW("Country", "%s", p.country);
        if (p.bint) ROW("Beacon int.", "%u TU (%u ms)", p.bint, (unsigned)((p.bint * 1024UL) / 1000));
    }
    if (p.rid) {                                        // ASTM F3411 Remote ID, decoded on the C5
        if (p.uas[0]) { ROW("UAS ID", "%s", p.uas); ROW("UA type", "%s", ua_type_str(p.uat)); }
        if (p.op[0])  ROW("Operator ID", "%s", p.op);
        if (p.loc)    ROW("Drone pos", "%.5f, %.5f", p.lat_e6 / 1e6, p.lon_e6 / 1e6);
        if (p.has_h)   ROW("Height AGL", "%.0f m", p.h_dm / 10.0);
        if (p.has_alt) ROW("Altitude", "%.0f m (WGS84)", p.alt_dm / 10.0);
        if (p.has_spd) ROW("Speed", "%.1f m/s (%.0f km/h)", p.spd_cms / 100.0, p.spd_cms * 0.036);
        if (p.has_vs)  ROW("Vert. speed", "%+.1f m/s", p.vs_cms / 100.0);
        if (p.has_hdg) ROW("Heading", "%d deg", (int)p.hdg);
        if (p.op_pos)  ROW("Pilot pos", "%.5f, %.5f", p.olat_e6 / 1e6, p.olon_e6 / 1e6);
    }
    (void)t;
}

// ---- focus: what INFO asks of the C5 -------------------------------------------------------------
bool ui_wants_focus(char* mac_out) {
    if (s_tab != TAB_INFO || !s_have_sel || s_diag || s_sel_gone) return false;
    strncpy(mac_out, s_sel_mac, 18);
    mac_out[17] = 0;
    return true;
}

// LINKS / TRAFF / RAW change with the traffic: poll quickly. INFO only needs the focus rows now and then.
// HUNT is fed by the C5's own RSSI ticks, so it needs no polling.
int ui_focus_view_period() { return (s_sub >= 1 && s_sub <= 3) ? 1000 : (s_sub == 0 ? 2000 : 0); }

static const char* vie_name(const uint8_t* o) {
    if (o[0] == 0x00 && o[1] == 0x50 && o[2] == 0xF2) return "Microsoft/WPA";
    if (o[0] == 0x00 && o[1] == 0x17 && o[2] == 0xF2) return "Apple";
    if (o[0] == 0x00 && o[1] == 0x10 && o[2] == 0x18) return "Broadcom";
    if (o[0] == 0x00 && o[1] == 0x03 && o[2] == 0x7F) return "Atheros/Qualcomm";
    if (o[0] == 0x50 && o[1] == 0x6F && o[2] == 0x9A) return "Wi-Fi Alliance";
    return nullptr;
}

static bool fv_for(const Det& d) { return g_fv.have && strcmp(g_fv.mac, d.mac) == 0; }

// Identity gathered by the parked radio (the C5 focus view), appended to INFO
static void add_focus_rows(const Det& d) {
    if (!fv_for(d)) { ROW("Focus", "engaging radio..."); return; }
    if (g_fv.wifi) {
        ROW("Focus", "locked on ch%d", g_fv.ch);
        if (g_fv.ht) ROW("Spatial streams", "%dx%d", g_fv.ht, g_fv.ht);
        ROW("Frames (focus)", "%u b=%u pr=%u", (unsigned)g_fv.n, (unsigned)g_fv.beacons, (unsigned)g_fv.presp);
        for (int i = 0; i < g_fv.nvie; i++) {
            const char* who = vie_name(g_fv.vie[i]);
            if (who) ROW("Vendor IE", "%s", who);
        }
        if (g_ui_set.opennet) {                             // open-network profiling: this device's own announcements
            const FvOnet* o = model_onet_for(d.mac);
            if (!o) ROW("Open net", "nothing announced yet");
            else {
                if (o->host[0]) ROW("DHCP hostname", "%s", o->host);
                if (o->vc[0]) ROW("DHCP class", "%s", o->vc);
                for (int i = 0; i < o->nnm; i++) ROW("mDNS name", "%s", o->nm[i]);
                for (int i = 0; i < o->nsv; i++) ROW("mDNS service", "%s", o->sv[i]);
                if (o->ss[0]) ROW("SSDP server", "%s", o->ss);
                ROW("IP traffic", "tcp %u udp %u icmp %u", (unsigned)o->tcp, (unsigned)o->udp, (unsigned)o->icmp);
                for (int i = 0; i < o->npt; i++) ROW("Port", "%u/%s x%u", o->pt[i][0], o->pt[i][1] == 6 ? "tcp" : "udp", o->pt[i][2]);
            }
        }
    } else {
        ROW("Focus", "BLE, full duty");
        ROW("Adverts (focus)", "%u", (unsigned)g_fv.advs);
        if (g_fv.adv_int) ROW("Advert interval", "~%u ms", (unsigned)g_fv.adv_int);
    }
}

static void build_rows(const Det& d, uint32_t now) {
    s_nrows = 0;
    char t[28];
    ROW("Type", "%s", TNAME[d.type]);
    ROW("Confidence", "%d%% %s", d.conf, d.conf >= 75 ? "Confirmed" : d.conf >= 45 ? "Likely" : "Possible");
    ROW("MAC", "%s", d.mac);
    if (const char* v = mac_vendor(d.mac)) ROW("MAC vendor", "%s", v);
    if (d.radio == 1) ROW("Radio", "BLE");
    else if (d.radio == 2) ROW("Radio", "Wi-Fi + BLE");
    else ROW("Radio", "Wi-Fi %s ch%d", d.band == 5 ? "5 GHz" : "2.4 GHz", d.ch);
    if (d.ssid[0]) ROW("SSID", "%s", d.ssid);
    ROW("RSSI now", "%d dBm", d.rssi);
    fmt_ago(t, sizeof(t), now - d.first_ms);
    ROW("First seen", "%s", t);
    fmt_ago(t, sizeof(t), now - d.last_ms);
    ROW("Last seen", "%s", t);
    ROW("Times heard", "%u", (unsigned)d.seen);
    ROW("Alert", "%s (strongest %d)", d.best >= CONF_ALERT_MIN ? "yes" : "no", d.best);
    ROW("Muted", "%s", d.muted ? "Yes" : "No");
    add_profile_rows(d);
    add_focus_rows(d);
}

// ---- LINKS / TRAFF / RAW pages ------------------------------------------------------------------
// Rows are rebuilt on every draw from the focus view (g_fv) and the detection model. Peer rows are
// tappable: the tap pivots focus to that device. Headers only: the C5 never sends a payload.
enum { LR_HEAD = 0, LR_INFO = 1, LR_PEER = 2 };
struct LRow {
    char     text[48];
    uint8_t  kind;
    uint8_t  radio;       // 0 wifi, 1 ble (for LR_PEER)
    uint8_t  ch;
    int8_t   rssi;
    char     mac[18];
    uint16_t color;
};
static LRow s_lrows[44];
static int  s_nl = 0;

static LRow* lrow_v(uint8_t kind, uint16_t color, const char* fmt, va_list ap) {
    if (s_nl >= (int)(sizeof(s_lrows) / sizeof(s_lrows[0]))) return nullptr;
    LRow& r = s_lrows[s_nl++];
    memset(&r, 0, sizeof(r));
    r.kind = kind; r.color = color; r.rssi = -100;
    vsnprintf(r.text, sizeof(r.text), fmt, ap);
    return &r;
}

static LRow* lrow(uint8_t kind, uint16_t color, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    LRow* r = lrow_v(kind, color, fmt, ap);
    va_end(ap);
    return r;
}

static void lrow_peer(uint16_t color, uint8_t radio, uint8_t ch, int rssi, const char* mac, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    LRow* r = lrow_v(LR_PEER, color, fmt, ap);
    va_end(ap);
    if (!r) return;
    r->radio = radio; r->ch = ch; r->rssi = (int8_t)constrain(rssi, -127, 0);
    strncpy(r->mac, mac, sizeof(r->mac) - 1);
}

static void fmt_bytes(char* out, size_t n, uint32_t b) {
    if (b < 1000)         snprintf(out, n, "%uB", (unsigned)b);
    else if (b < 1000000) snprintf(out, n, "%uK", (unsigned)(b / 1000));
    else                  snprintf(out, n, "%.1fM", b / 1e6f);
}

static bool parse_mac6(const char* s, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

// Same device family: octets 0-4 equal (ignoring the locally-administered bit), last octet close.
// That is how multi-radio / multi-SSID gear allocates its base MACs.
static bool same_block(const uint8_t* a, const uint8_t* b) {
    if ((a[0] | 0x02) != (b[0] | 0x02)) return false;
    for (int i = 1; i < 5; i++) if (a[i] != b[i]) return false;
    return abs((int)a[5] - (int)b[5]) <= 16;
}

static void build_links(const Det& d) {
    s_nl = 0;
    bool fvm = fv_for(d);
    lrow(LR_HEAD, C_SIGNAL, "OBSERVED");
    bool any = false;
    if (d.radio == 2) { lrow(LR_INFO, C_WHITE, "Same MAC heard on BLE and Wi-Fi"); any = true; }
    if (g_profile.have && strcmp(g_profile.mac, d.mac) == 0 && g_profile.probe[0]) {
        lrow(LR_INFO, C_WHITE, "Probes for: %s", g_profile.probe); any = true;
    }
    if (fvm && g_fv.wifi) {
        for (int i = 0; i < g_fv.npeers; i++) {
            const FvPeer& p = g_fv.peers[i];
            if (p.role == 0) continue;
            const char* pv = mac_vendor(p.mac);
            lrow_peer(C_ACCENT, 0, g_fv.ch, p.rssi, p.mac, "%s %s %.10s", p.role == 2 ? "AP " : "STA", p.mac, pv ? pv : "");
            any = true;
        }
        if (!any) lrow(LR_INFO, C_MDGREY, "No AP/client links seen yet");
    } else if (fvm) {
        if (!any) lrow(LR_INFO, C_MDGREY, "Nothing observable yet");
        lrow(LR_INFO, C_MDGREY, "BLE pairings/connections hop channels");
        lrow(LR_INFO, C_MDGREY, "this radio can't follow: not visible");
    } else {
        lrow(LR_INFO, C_MDGREY, "Collecting from target...");
    }

    lrow(LR_HEAD, C_ATTACK, "INFERRED");
    bool inf = false;
    uint8_t tm[6];
    if (parse_mac6(d.mac, tm)) {
        for (int i = 0; i < g_model.ndets; i++) {
            const Det& e = g_model.dets[i];
            uint8_t em[6];
            if (!e.used || strcmp(e.mac, d.mac) == 0 || !parse_mac6(e.mac, em) || !same_block(em, tm)) continue;
            lrow_peer(C_ATTACK, e.radio == 1 ? 1 : 0, e.ch, e.rssi, e.mac, "SIB %s %.18s", e.mac, e.ssid[0] ? e.ssid : e.name);
            inf = true;
        }
    }
    if (!inf) lrow(LR_INFO, C_MDGREY, "Nothing inferred yet");

    lrow(LR_HEAD, C_MDGREY, "CORRELATED");
    lrow(LR_INFO, C_MDGREY, "Needs session history on SD (planned)");
}

static void build_traffic(const Det& d) {
    s_nl = 0;
    if (!fv_for(d)) { lrow(LR_INFO, C_MDGREY, "Collecting from target..."); return; }
    if (!g_fv.wifi) {
        lrow(LR_INFO, C_MDGREY, "BLE target: adverts only. Connections");
        lrow(LR_INFO, C_MDGREY, "hop channels and can't be followed.");
        lrow(LR_INFO, C_WHITE, "Adverts heard: %u", (unsigned)g_fv.advs);
        return;
    }
    lrow(LR_HEAD, C_SIGNAL, "data %u  mgmt %u  peers %u", (unsigned)g_fv.data, (unsigned)g_fv.n, g_fv.npeers);
    for (int i = 0; i < g_fv.npeers; i++) {                 // the C5 sends them busiest first
        const FvPeer& p = g_fv.peers[i];
        char b[10], r[8] = "";
        fmt_bytes(b, sizeof(b), p.tob + p.frb);
        if (p.frf) snprintf(r, sizeof(r), " %d", p.rssi);
        lrow_peer(C_WHITE, 0, g_fv.ch, p.rssi, p.mac, "%s >%u <%u %s%s%s", p.mac, (unsigned)p.tof, (unsigned)p.frf, b,
                  p.qos ? " Q" : "", r);
    }
    if (g_fv.npeers == 0) {
        lrow(LR_INFO, C_MDGREY, "No peers yet. Needs a busy target:");
        lrow(LR_INFO, C_MDGREY, "an AP with clients, or an active client.");
    }
    if (g_ui_set.opennet) {                                 // open-network profiling: who announced what (SET > PROFILE)
        lrow(LR_HEAD, C_ATTACK, "OPEN NET: announced");
        if (!g_fv.nonet) lrow(LR_INFO, C_MDGREY, "Waiting for DHCP / mDNS / SSDP (open nets only)");
        for (int i = 0; i < g_fv.nonet; i++) {
            const FvOnet& o = g_fv.onet[i];
            const char* who = o.host[0] ? o.host : o.nnm ? o.nm[0] : o.ss[0] ? o.ss : o.vc[0] ? o.vc : "";
            int pr = -100;                                  // the peer's own RSSI from the peer list, if it is there
            for (int k = 0; k < g_fv.npeers; k++) if (strcasecmp(g_fv.peers[k].mac, o.mac) == 0) pr = g_fv.peers[k].rssi;
            lrow_peer(C_ATTACK, 0, g_fv.ch, pr, o.mac, "%s %.20s", o.mac, who);
        }
    }
}

static void hexline(char* out, size_t cap, const uint8_t* p, int n) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k + 4 < cap; i++) k += snprintf(out + k, cap - k, "%02X ", p[i]);
    if (k) out[k - 1] = 0;
}

static const char* ad_name(uint8_t t) {
    switch (t) {
        case 0x01: return "flags";   case 0x02: case 0x03: return "uuid16";  case 0x06: case 0x07: return "uuid128";
        case 0x08: case 0x09: return "name"; case 0x0A: return "txpower";    case 0x16: return "svcdata";
        case 0x19: return "appear";  case 0xFF: return "mfg";                default: return "type";
    }
}

static void build_raw(const Det& d) {
    s_nl = 0;
    if (!fv_for(d)) { lrow(LR_INFO, C_MDGREY, "Collecting from target..."); return; }
    char tmp[48];
    if (!g_fv.wifi) {
        if (!g_fv.raw_len) { lrow(LR_INFO, C_MDGREY, "No advert captured yet"); return; }
        lrow(LR_HEAD, C_SIGNAL, "ADVERT PAYLOAD (%u bytes)", g_fv.raw_len);
        for (int off = 0; off < g_fv.raw_len; off += 14) {
            hexline(tmp, sizeof(tmp), g_fv.raw + off, min(14, g_fv.raw_len - off));
            lrow(LR_INFO, C_WHITE, "%s", tmp);
        }
        lrow(LR_HEAD, C_ACCENT, "DECODED");
        int i = 0;
        while (i + 1 < g_fv.raw_len) {
            int l = g_fv.raw[i];
            if (l == 0 || i + 1 + l > g_fv.raw_len) break;
            uint8_t t = g_fv.raw[i + 1];
            int dl = l - 1;
            hexline(tmp, sizeof(tmp), g_fv.raw + i + 2, min(dl, 8));
            lrow(LR_INFO, C_LTGREY, "%02X %-8s %s%s", t, ad_name(t), tmp, dl > 8 ? ".." : "");
            i += 1 + l;
        }
        return;
    }
    if (!g_fv.has_iemap) { lrow(LR_INFO, C_MDGREY, "No beacon / probe frame from the"); lrow(LR_INFO, C_MDGREY, "target captured yet."); return; }
    lrow(LR_HEAD, C_SIGNAL, "ELEMENT IDS SEEN");
    char line[44] = "";
    for (int id = 0; id < 256; id++) {
        if (!(g_fv.iemap[id >> 3] & (1 << (id & 7)))) continue;
        char t[6];
        snprintf(t, sizeof(t), "%d ", id);
        if (strlen(line) + strlen(t) >= 40) { lrow(LR_INFO, C_WHITE, "%s", line); line[0] = 0; }
        strncat(line, t, sizeof(line) - strlen(line) - 1);
    }
    if (line[0]) lrow(LR_INFO, C_WHITE, "%s", line);
    if (g_fv.ht) lrow(LR_INFO, C_LTGREY, "HT spatial streams: %u", g_fv.ht);
    lrow(LR_HEAD, C_ACCENT, "VENDOR IES");
    for (int i = 0; i < g_fv.nvie; i++) {
        const uint8_t* o = g_fv.vie[i];
        const char* who = vie_name(o);
        lrow(LR_INFO, C_WHITE, "%02X:%02X:%02X t%02X %s", o[0], o[1], o[2], o[3], who ? who : "");
    }
    if (g_fv.nvie == 0) lrow(LR_INFO, C_MDGREY, "none");
}

static void draw_rows_generic() {
    int maxs = max(0, s_nl - DVIS);
    if (s_scroll > maxs) s_scroll = maxs;
    int lw = SCREEN_W - DSTRIP_W;
    for (int r = 0; r < DVIS; r++) {
        int y = DLIST_Y + r * DROW_H, i = s_scroll + r;
        uint16_t bg = (i % 2 == 0) ? C_PANEL : C_PANEL2;
        if (i >= s_nl) { tft.fillRect(0, y, lw, DROW_H - 1, C_BLACK); continue; }
        const LRow& row = s_lrows[i];
        if (row.kind == LR_HEAD) bg = C_BLACK;
        tft.fillRect(0, y, lw, DROW_H - 1, bg);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(row.color, bg);
        tft.drawString(row.text, row.kind == LR_HEAD ? 4 : 8, y + DROW_H / 2, 1);
        if (row.kind == LR_PEER) {                             // chevron: tap to pivot
            int cx = lw - 8, cy = y + DROW_H / 2;
            tft.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 3, cy, C_ACCENT);
        }
    }
    int sx = SCREEN_W - DSTRIP_W;
    tft.fillRect(sx, DLIST_Y, DSTRIP_W, DLIST_H, C_BLACK);
    uint16_t uc = s_scroll > 0 ? C_ACCENT : C_BORDER, dc = s_scroll < maxs ? C_ACCENT : C_BORDER;
    int cx = sx + DSTRIP_W / 2;
    tft.fillTriangle(cx, DLIST_Y + 2, cx - 6, DLIST_Y + 12, cx + 6, DLIST_Y + 12, uc);
    tft.fillTriangle(cx, DLIST_Y + DLIST_H - 2, cx - 6, DLIST_Y + DLIST_H - 12, cx + 6, DLIST_Y + DLIST_H - 12, dc);
}

static void draw_link_page(const Det& d) {
    if (s_sub == 1) build_links(d);
    else if (s_sub == 2) build_traffic(d);
    else build_raw(d);
    draw_rows_generic();
}

// Focus the device behind a tapped LINKS / TRAFF row. The C5 lists a Wi-Fi peer it has never matched as
// a low-confidence "Peer (pivot)" entry, so INFO / RAW / HUNT work on it like on any detection.
static void pivot_to(const LRow& r) {
    Det* nd = model_add_peer(r.mac, r.radio, r.ch, r.rssi);
    if (!nd) { toast("detection list full: can't pivot"); return; }
    strncpy(s_sel_mac, r.mac, sizeof(s_sel_mac) - 1);
    s_sel_mac[sizeof(s_sel_mac) - 1] = 0;
    s_have_sel = true; s_sel_gone = false;
    s_sub = 0; s_scroll = 0; s_dirty = true;
}

// ---- HUNT ----------------------------------------------------------------------------------------
// Geiger-style walk-down on the focused target: click rate, screen ring and RGB LED all speed up with
// RSSI, and go quiet after HUNT_SILENCE_MS without contact. RSSI arrives as the C5's RSSI_TICK events.
#define HUNT_SILENCE_MS 8000
#define HUNT_RSSI_FLOOR -95
#define HUNT_RSSI_CEIL  -35

static bool     s_hunt_active = false;
static uint32_t s_hunt_last_click = 0, s_hunt_last_pulse = 0, s_hunt_draw_ms = 0;
static bool     s_hunt_pulse_state = false;
static int      s_hunt_peak = -128, s_hunt_prev_rssi = -128;
static char     s_hunt_peak_mac[18] = {};

static float hunt_t(int rssi) {
    int r = constrain(rssi, HUNT_RSSI_FLOOR, HUNT_RSSI_CEIL);
    return (float)(r - HUNT_RSSI_FLOOR) / (HUNT_RSSI_CEIL - HUNT_RSSI_FLOOR);       // 0 far .. 1 near
}
static uint32_t hunt_interval(int rssi) { return (uint32_t)(1200 - hunt_t(rssi) * 1130); }   // 1.2 s far .. 70 ms near

static uint16_t hunt_heat(int rssi) {                                              // blue -> cyan -> amber -> red
    float t = hunt_t(rssi);
    uint8_t R, G, B;
    if (t < 0.5f) { float u = t / 0.5f; R = 0; G = (uint8_t)(128 + 127 * u); B = (uint8_t)(255 - 100 * u); }
    else { float u = (t - 0.5f) / 0.5f; R = (uint8_t)(255 * u); G = (uint8_t)(255 - 100 * u); B = (uint8_t)(155 - 155 * u); }
    return tft.color565(R, G, B);
}

static void hunt_led(bool on, int rssi) {                                          // active LOW
    float t = hunt_t(rssi);
    digitalWrite(PIN_LED_R, (on && t > 0.4f) ? LOW : HIGH);
    digitalWrite(PIN_LED_G, (on && t < 0.6f) ? LOW : HIGH);
    digitalWrite(PIN_LED_B, (on && t < 0.3f) ? LOW : HIGH);
}

static void hunt_all_off() {
    if (!s_beep_until) { ledcWriteTone(1, 0); ledcWrite(1, 0); }                  // never cut an alert tone short
    if (!s_led_until) { digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH); }
}

// Latest reading for the target: the C5's tick when it matches, else what the list last said
static bool hunt_sample(const Det& d, bool gone, int* rssi, uint32_t* age) {
    uint32_t now = millis();
    if (g_hunt.ever && strcmp(g_hunt.mac, d.mac) == 0) { *age = now - g_hunt.heard_ms; *rssi = g_hunt.rssi; }
    else { *age = now - d.last_ms; *rssi = d.rssi; }
    return !gone && *age < HUNT_SILENCE_MS;
}

static void hunt_tick(const Det& d, bool gone) {
    uint32_t now = millis();
    int rssi; uint32_t age;
    bool live = hunt_sample(d, gone, &rssi, &age);
    s_hunt_active = true;
    if (strcmp(s_hunt_peak_mac, d.mac) != 0) {                                      // new target: reset the trend
        strncpy(s_hunt_peak_mac, d.mac, sizeof(s_hunt_peak_mac) - 1);
        s_hunt_peak = -128; s_hunt_prev_rssi = -128;
    }
    if (live && rssi > s_hunt_peak) s_hunt_peak = rssi;
    if (!live) { hunt_all_off(); s_hunt_pulse_state = false; return; }
    uint32_t iv = hunt_interval(rssi);
    if (g_ui_set.speaker && now - s_hunt_last_click >= iv) { s_hunt_last_click = now; ledcWriteTone(1, 3000); }
    else if (now - s_hunt_last_click >= 12 && !s_beep_until) ledcWrite(1, 0);       // end the click quickly
    if (now - s_hunt_last_pulse >= iv) {
        s_hunt_last_pulse = now;
        s_hunt_pulse_state = !s_hunt_pulse_state;
        if (g_ui_set.led && !s_led_until) hunt_led(s_hunt_pulse_state, rssi);
    }
}

static void draw_hunt_body(const Det& d, bool gone) {
    int rssi; uint32_t age;
    bool live = hunt_sample(d, gone, &rssi, &age);
    uint16_t heat = live ? hunt_heat(rssi) : C_PANEL;
    int cy = (DHEAD_Y + TAB_Y) / 2;

    int ringR = 46;
    uint16_t ringc = (live && s_hunt_pulse_state) ? heat : C_BLACK;                  // strobes with the clicks
    tft.fillCircle(SCREEN_W / 2, cy, ringR, ringc);
    tft.drawCircle(SCREEN_W / 2, cy, ringR, live ? heat : C_BORDER);

    char v[12];
    if (live) snprintf(v, sizeof(v), "%d", rssi); else snprintf(v, sizeof(v), "--");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(live ? C_BLACK : C_MDGREY, ringc);
    tft.drawString(v, SCREEN_W / 2, cy - 6, 6);
    tft.drawString("dBm", SCREEN_W / 2, cy + 20, 2);

    tft.setTextDatum(ML_DATUM);
    tft.fillRect(4, DHEAD_Y + 6, 70, 60, C_BLACK);
    const char* arrow = "--";
    uint16_t ac = C_MDGREY;
    if (live) {
        if (rssi > s_hunt_prev_rssi + 1) { arrow = "WARMER"; ac = C_TRACKER; }
        else if (rssi < s_hunt_prev_rssi - 1) { arrow = "colder"; ac = C_ACCENT; }
        else { arrow = "steady"; ac = C_LTGREY; }
        s_hunt_prev_rssi = rssi;
    }
    tft.setTextColor(ac, C_BLACK);
    tft.drawString(arrow, 6, DHEAD_Y + 16, 2);
    char pk[16];
    snprintf(pk, sizeof(pk), "peak %d", s_hunt_peak > -128 ? s_hunt_peak : 0);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.drawString(pk, 6, DHEAD_Y + 36, 2);

    tft.setTextDatum(MR_DATUM);
    tft.fillRect(SCREEN_W - 78, DHEAD_Y + 6, 74, 60, C_BLACK);
    char ls[18];
    if (gone) snprintf(ls, sizeof(ls), "aged out");
    else if (!g_hunt.ever && !live) snprintf(ls, sizeof(ls), "waiting");
    else snprintf(ls, sizeof(ls), "%us ago", (unsigned)(age / 1000));
    tft.setTextColor(live ? C_LTGREY : C_CAMERA, C_BLACK);
    tft.drawString(ls, SCREEN_W - 6, DHEAD_Y + 16, 2);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.drawString(g_ui_set.speaker ? "sound on" : "muted", SCREEN_W - 6, DHEAD_Y + 36, 2);
}

static const char* SUB_NAMES[] = { "INFO", "LINKS", "TRAFF", "RAW", "HUNT", "CAP" };

static void draw_info() {
    if (!s_have_sel) {
        clear_page();
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Select a detection on LIST", SCREEN_W / 2, PAGE_Y + PAGE_H / 2, 2);
        return;
    }
    Det* live = model_find(s_sel_mac);
    bool gone = (live == nullptr);
    if (live) s_sel_last = *live;
    if (gone != s_sel_gone) { s_sel_gone = gone; s_dirty = true; }
    const Det& d = s_sel_last;
    uint16_t tc = TCOL[d.type];

    // Title strip: Back, name, Mute
    tft.fillRect(0, PAGE_Y, SCREEN_W, SUBTAB_Y - PAGE_Y, C_BLACK);
    tft.setTextColor(C_ACCENT, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("< Back", 4, PAGE_Y + 10, 2);
    if (!gone) {
        tft.fillRect(SCREEN_W - 72, PAGE_Y + 2, 70, 22, C_PANEL);
        tft.drawRect(SCREEN_W - 72, PAGE_Y + 2, 70, 22, C_CAMERA);
        tft.setTextColor(C_CAMERA, C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(d.muted ? "Unmute" : "Mute", SCREEN_W - 37, PAGE_Y + 13, 1);
    } else {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MR_DATUM);
        tft.drawString("aged out", SCREEN_W - 4, PAGE_Y + 10, 1);
    }
    char nm[26]; snprintf(nm, sizeof(nm), "%s", d.name);
    int n = strlen(nm);
    while (n > 2 && tft.textWidth(nm, 1) > SCREEN_W - 150) { nm[--n] = 0; nm[n - 1] = '.'; nm[n - 2] = '.'; }
    tft.setTextColor(gone ? C_MDGREY : tc, C_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(nm, SCREEN_W / 2, PAGE_Y + 12, 1);

    // Sub-tabs: INFO LINKS TRAFF RAW HUNT are live; CAP needs the SD capture (not built)
    int w = SCREEN_W / 6;
    for (int i = 0; i < 6; i++) {
        bool on = (i == s_sub);
        tft.fillRect(i * w, SUBTAB_Y, w - 1, SUBTAB_H - 1, on ? tc : C_PANEL);
        tft.setTextColor(on ? C_BLACK : (i == 5 ? C_BORDER : C_LTGREY), on ? tc : C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(SUB_NAMES[i], i * w + w / 2, SUBTAB_Y + SUBTAB_H / 2 - 1, 1);
    }

    if (s_sub >= 1 && s_sub <= 3) { draw_link_page(d); return; }
    if (s_sub == 4) { draw_hunt_body(d, gone); return; }

    build_rows(d, millis());
    int maxs = max(0, s_nrows - DVIS);
    if (s_scroll > maxs) s_scroll = maxs;
    int lw = SCREEN_W - DSTRIP_W;
    for (int r = 0; r < DVIS; r++) {
        int y = DLIST_Y + r * DROW_H, i = s_scroll + r;
        uint16_t bg = (i % 2 == 0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, y, lw, DROW_H - 1, i < s_nrows ? bg : C_BLACK);
        if (i >= s_nrows) continue;
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_MDGREY, bg);
        tft.drawString(s_rows[i].k, 4, y + DROW_H / 2, 1);
        tft.setTextColor(gone ? C_LTGREY : C_WHITE, bg);
        char buf[44]; strncpy(buf, s_rows[i].v, sizeof(buf));
        int bl = strlen(buf);
        while (bl > 2 && tft.textWidth(buf, 2) > lw - DVAL_X - 2) { buf[--bl] = 0; buf[bl - 1] = '.'; buf[bl - 2] = '.'; }
        tft.drawString(buf, DVAL_X, y + DROW_H / 2, 2);
    }
    int sx = SCREEN_W - DSTRIP_W;
    tft.fillRect(sx, DLIST_Y, DSTRIP_W, DLIST_H, C_BLACK);
    uint16_t uc = s_scroll > 0 ? C_ACCENT : C_BORDER, dc = s_scroll < maxs ? C_ACCENT : C_BORDER;
    int cx = sx + DSTRIP_W / 2;
    tft.fillTriangle(cx, DLIST_Y + 2, cx - 6, DLIST_Y + 12, cx + 6, DLIST_Y + 12, uc);
    tft.fillTriangle(cx, DLIST_Y + DLIST_H - 2, cx - 6, DLIST_Y + DLIST_H - 12, cx + 6, DLIST_Y + DLIST_H - 12, dc);
}

// ---- SET -------------------------------------------------------------------------------------
static const char* TILE_LABEL[] = { "Drones", "Trackers", "Cameras", "BodyCams", "Attack", "Speaker", "LED" };
static const char* TILE_SUB[]   = { "Remote ID", "AirTag/Tile", "Flock ALPR", "Axon", "Flipper", "", "" };

static bool tile_value(int i) {
    return i < 5 ? g_ui_set.detect[i] : (i == 5 ? g_ui_set.speaker : g_ui_set.led);
}

static void draw_toggle(int x, int y, int w, int h, const char* label, const char* sub, bool on) {
    tft.fillRect(x, y, w, h, C_PANEL);
    tft.drawRect(x, y, w, h, C_BORDER);
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(label, x + 4, y + 8, 2);
    if (sub[0]) { tft.setTextColor(C_MDGREY, C_PANEL); tft.drawString(sub, x + 4, y + h - 9, 1); }
    tft.fillRoundRect(x + w - 36, y + h / 2 - 8, 30, 16, 8, on ? C_SIGNAL : C_DKGREY);
    tft.fillCircle(on ? x + w - 16 : x + w - 31, y + h / 2, 5, C_WHITE);
}

static void draw_button(int x, int y, int w, int h, const char* label, uint16_t col) {
    tft.fillRoundRect(x, y, w, h, 6, C_PANEL);
    tft.drawRoundRect(x, y, w, h, 6, col);
    tft.setTextColor(col, C_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + w / 2, y + h / 2, 2);
}

#define SET_SEG_H 18
#define DEV_Y0   (PAGE_Y + SET_SEG_H + 6)
#define DEV_ROW  38

// ABOUT: project URL, credits (from CREDITS.md) and the firmware versions. Every line is drawn with an opaque
// background, so the periodic redraw is flicker-free; the C5 line follows the link.
static void draw_about(const UiCtx& c) {
    int y = DEV_Y0 - 2;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_ACCENT, C_BLACK);
    tft.drawString("SignalSweep CYD", 4, y, 2);
    y += 18;
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.drawString("github.com/pbarsamian/SignalSweep-CYD", 4, y, 1);
    y += 14;
    tft.setTextColor(C_LTGREY, C_BLACK);
    for (int i = 0; i < ABOUT_CREDITS_N; i++, y += 10) tft.drawString(ABOUT_CREDITS[i], 4, y, 1);
    y += 6;
    char b[48];
    tft.fillRect(0, y, SCREEN_W, 34, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    snprintf(b, sizeof(b), "CYD firmware   %s", FW_VERSION);
    tft.drawString(b, 4, y, 1);
    snprintf(b, sizeof(b), "C5 firmware    %s", c.c5->link_up && c.c5->fw[0] ? c.c5->fw : "not linked");
    tft.setTextColor(c.c5->link_up ? C_WHITE : C_MDGREY, C_BLACK);
    tft.drawString(b, 4, y + 11, 1);
    snprintf(b, sizeof(b), "Protocol       %u", (unsigned)SS_PROTO_VERSION);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.drawString(b, 4, y + 22, 1);
}

// PROFILE: the one switch for open-network profiling (issue #11). Plain statement of what it does.
static void draw_profile() {
    int y = DEV_Y0;
    draw_toggle(2, y, SCREEN_W - 4, 38, "Open-network profiling", g_ui_set.opennet ? "ON until reboot" : "OFF", g_ui_set.opennet);
    static const char* const T[] = {
        "Passive. Runs only while focused on a Wi-Fi target,",
        "and only reads UNENCRYPTED frames.",
        "Keeps: DHCP hostname + vendor class, mDNS and",
        "SSDP announcements, protocols and ports.",
        "Never: payloads, DNS, web addresses, TLS names.",
        "Held in RAM, dropped when the focus is released.",
        "Shown on INFO of a peer and on the TRAFF page.",
        "Off at every boot. Encrypted networks show nothing.",
    };
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LTGREY, C_BLACK);
    for (int i = 0; i < 8; i++) tft.drawString(T[i], 4, y + 46 + i * 11, 1);
}

static void draw_set(const UiCtx& c) {
    // Segmented control: ALERTS | DEVICE | PROFILE | ABOUT
    static const char* const SEG[4] = { "ALERTS", "DEVICE", "PROFILE", "ABOUT" };
    for (int i = 0; i < 4; i++) {
        bool on = (s_set_page == i);
        int x = i * (SCREEN_W / 4);
        tft.fillRect(x, PAGE_Y, SCREEN_W / 4 - 1, SET_SEG_H - 2, on ? C_ACCENT : C_PANEL);
        tft.setTextColor(on ? C_BLACK : C_LTGREY, on ? C_ACCENT : C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(SEG[i], x + SCREEN_W / 8, PAGE_Y + SET_SEG_H / 2 - 1, 1);
    }
    if (s_set_page == 3) { draw_about(c); return; }
    if (s_set_page == 2) { draw_profile(); return; }
    if (s_set_page == 0) {
        for (int i = 0; i < 7; i++) {
            int col = (i < 4) ? 0 : 1, row = (i < 4) ? i : (i - 4);
            draw_toggle(col * 162 + 2, PAGE_Y + SET_SEG_H + 4 + row * 42, 158, 38, TILE_LABEL[i], TILE_SUB[i], tile_value(i));
        }
        char sl[24];
        snprintf(sl, sizeof(sl), "SCAN: %s", SCHED_PRESETS[g_ui_set.sched].name);
        draw_button(164, PAGE_Y + SET_SEG_H + 4 + 3 * 42, 154, 38, sl, C_ACCENT);   // tap: next Wi-Fi / BLE balance
        return;
    }
    // DEVICE page
    int y = DEV_Y0;
    tft.fillRect(0, y, SCREEN_W, 2 * DEV_ROW + 2 * DEV_ROW, C_BLACK);
    // RF mode (read-only until the consent flow exists)
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("RF MODE", 6, y + 10, 2);
    uint16_t pc = (c.mode == SS_MODE_SILENT_RX) ? C_SIGNAL : C_WARN;
    tft.fillRoundRect(100, y + 2, 90, 18, 8, pc);
    tft.setTextColor(C_BLACK, pc);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(mode_name(c.mode), 145, y + 11, 1);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("TX needs the consent flow (not built yet)", 6, y + 28, 1);
    y += DEV_ROW;
    draw_button(6, y, SCREEN_W - 12, 30, "CHARGE MODE", C_ACCENT);
    y += DEV_ROW;
    // Brightness
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("BRIGHTNESS", 6, y + 14, 2);
    draw_button(120, y, 40, 30, "-", C_LTGREY);
    draw_button(SCREEN_W - 46, y, 40, 30, "+", C_LTGREY);
    int bw = SCREEN_W - 46 - 6 - 160 - 6;                          // gap between the - and + buttons
    tft.drawRect(166, y + 8, bw, 14, C_BORDER);
    tft.fillRect(167, y + 9, ((bw - 2) * (g_ui_set.brightness - 25)) / 230, 12, C_ACCENT);
    y += DEV_ROW;
    draw_button(6, y, SCREEN_W - 12, 30, "DIAGNOSTICS", C_LTGREY);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    { char f[64]; snprintf(f, sizeof(f), "SignalSweep CYD %s  GPL-3.0  based on B0z0dcl0wn/SignalSweep", FW_VERSION); tft.drawString(f, 4, TAB_Y - 8, 1); }
}

// ---- update / touch ----------------------------------------------------------------------------
static void draw_toast() {
    if (!s_toast[0]) return;
    if ((int32_t)(millis() - s_toast_until) >= 0) { s_toast[0] = 0; s_dirty = true; return; }
    tft.fillRect(0, TAB_Y - 20, SCREEN_W, 18, C_PANEL);
    tft.drawRect(0, TAB_Y - 20, SCREEN_W, 18, C_ACCENT);
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(s_toast, SCREEN_W / 2, TAB_Y - 11, 1);
}

void ui_update(const UiCtx& c) {
    uint32_t now = millis();
    alert_tick(now);
    model_prune(now);
    s_scan = c.c5->link_up ? c.c5->scan : 3;
    s_lock = c.c5->link_up ? c.c5->lock : 0;
    if (s_scope_pref == SC_AUTO && c.c5->link_up) {          // the scope follows what is actually being scanned
        int want;
        if (s_scan == 2) want = 0;                            // BLE has the radio: the BLE list
        else if (s_scan == 1) want = c.c5->band == 5 ? 5 : 2; // Wi-Fi alone (a slot, or a focus lock): the band it is on
        else {                                                // both run together: rotate 2.4 -> 5 -> BLE, 6 s each
            static uint32_t rot_ms = 0;
            static int rot = 0;
            if (now - rot_ms >= 6000) { rot_ms = now; rot = (rot + 1) % 3; }
            want = rot == 0 ? 2 : rot == 1 ? 5 : 0;
        }
        if (want != s_scope_band) { s_scope_band = want; if (s_tab == TAB_SCOPE) s_dirty = true; }
    }

    if (s_diag) {                                   // full-screen bring-up page
        if (now - s_drawn_ms >= 250) { s_drawn_ms = now; status_ui_draw(*c.diag, c.mode); }
        return;
    }
    draw_header(c);
    static int last_tab = -1;
    if (last_tab != s_tab || s_dirty) draw_tabs();

    uint32_t period = (s_tab == TAB_SCOPE) ? 500 : (s_tab == TAB_INFO ? 500 : 1000);
    bool due = s_dirty || (now - s_drawn_ms >= period);
    if (last_tab != s_tab) { last_tab = s_tab; clear_page(); due = true; }
    if (s_tab == TAB_INFO && s_have_sel && s_sub == 4 && !s_diag) {          // HUNT: clicks and ring run at their own pace
        Det* hd = model_find(s_sel_mac);
        const Det& hr = hd ? *hd : s_sel_last;
        hunt_tick(hr, hd == nullptr);
        if (!due && now - s_hunt_draw_ms >= 100) { s_hunt_draw_ms = now; draw_hunt_body(hr, hd == nullptr); }
    } else if (s_hunt_active) { hunt_all_off(); s_hunt_pulse_state = false; s_hunt_active = false; }
    if (due) {
        s_drawn_ms = now;
        if (s_dirty) clear_page();
        s_dirty = false;
        switch (s_tab) {
            case TAB_SCOPE: draw_scope(); break;
            case TAB_LIST:  draw_list(); break;
            case TAB_INFO:  draw_info(); break;
            case TAB_SET:   if (s_set_page == 0 || due) draw_set(c); break;
        }
    }
    draw_toast();
}

static void apply_brightness() {
    g_backlight = g_ui_set.brightness;
    ledcWrite(0, g_backlight);
    settings_save();
}

void ui_touch(int tx, int ty) {
    if (s_diag) { s_diag = false; ui_invalidate(); return; }

    if (ty >= TAB_Y) {                              // tab bar
        int t = tx / TAB_W;
        if (t >= 0 && t < 4 && t != s_tab) { s_tab = t; s_scroll = 0; s_dirty = true; s_toast[0] = 0; }
        return;
    }
    if (s_tab == TAB_SCOPE) {
        if (in_rect(tx, ty, SCREEN_W - 6 - 58, PAGE_Y + 3, 60, 16)) {      // AUTO -> 2.4 -> 5 -> BLE -> AUTO
            s_scope_pref = (s_scope_pref == SC_AUTO) ? 2 : (s_scope_pref == 2) ? 5 : (s_scope_pref == 5) ? 0 : SC_AUTO;
            if (s_scope_pref != SC_AUTO) s_scope_band = s_scope_pref;
            s_dirty = true;
            return;
        }
        // tap a detection row under the scope: open it
        int ry0 = PAGE_Y + 1 + 84 + 32;
        if (ty >= ry0 && s_scope_band != 0) {                  // an access point row: open it and focus it
            int order[MODEL_MAX_APS];
            int n = ap_order(s_scope_band, order), k = (ty - ry0) / ROW_H;
            if (k < n) {
                const ApRow& a = g_model.aps[order[k]];
                Det* d = model_add_peer(a.mac, 0, a.ch, a.rssi);
                if (!d) { toast("detection list full: can't open it"); return; }
                if (d->type == T_UNKNOWN) {                        // a peer placeholder: name it after the network
                    snprintf(d->name, sizeof(d->name), "%s", a.ssid[0] ? a.ssid : "AP (hidden)");
                    strncpy(d->ssid, a.ssid, sizeof(d->ssid) - 1);
                }
                strncpy(s_sel_mac, d->mac, sizeof(s_sel_mac) - 1);
                s_have_sel = true; s_sel_gone = false;
                s_tab = TAB_INFO; s_scroll = 0; s_sub = 0; s_dirty = true;
            }
            return;
        }
        if (ty >= ry0) {
            int i = visible_index((ty - ry0) / ROW_H);
            if (i >= 0) { strncpy(s_sel_mac, g_model.dets[i].mac, sizeof(s_sel_mac) - 1); s_have_sel = true; s_tab = TAB_INFO; s_scroll = 0; s_sub = 0; s_dirty = true; }
        }
        return;
    }
    if (s_tab == TAB_LIST) {
        int total = model_visible_count();
        if (tx >= SCREEN_W - 14) {
            int mid = PAGE_Y + (PAGE_H - 36) / 2;
            if (ty < mid && s_scroll > 0) s_scroll--;
            else if (ty >= mid && ty < TAB_Y - 10 && s_scroll + ROWS_VIS < total) s_scroll++;
            s_dirty = true;
            return;
        }
        int i = visible_index(s_scroll + (ty - PAGE_Y) / ROW_H);
        if (i >= 0) { strncpy(s_sel_mac, g_model.dets[i].mac, sizeof(s_sel_mac) - 1); s_have_sel = true; s_tab = TAB_INFO; s_scroll = 0; s_sub = 0; s_dirty = true; }
        return;
    }
    if (s_tab == TAB_INFO && s_have_sel) {
        if (in_rect(tx, ty, 0, PAGE_Y, 80, 22)) { s_tab = TAB_LIST; s_scroll = 0; s_dirty = true; return; }
        if (in_rect(tx, ty, SCREEN_W - 72, PAGE_Y, 70, 22) && !s_sel_gone) {
            Det* d = model_find(s_sel_mac);
            if (d) d->muted = !d->muted;
            s_dirty = true;
            return;
        }
        if (ty >= SUBTAB_Y && ty < SUBTAB_Y + SUBTAB_H) {
            int p = tx / (SCREEN_W / 6);
            if (p == 5) toast("CAP needs the SD capture (not built yet)");
            else if (p >= 0 && p < 5 && p != s_sub) { s_sub = p; s_scroll = 0; s_dirty = true; }
            return;
        }
        if ((s_sub == 1 || s_sub == 2) && tx < SCREEN_W - DSTRIP_W && ty >= DLIST_Y && ty < DLIST_Y + DLIST_H) {
            int i = s_scroll + (ty - DLIST_Y) / DROW_H;
            if (i >= 0 && i < s_nl && s_lrows[i].kind == LR_PEER) pivot_to(s_lrows[i]);
            return;
        }
        if (tx >= SCREEN_W - DSTRIP_W && ty >= DLIST_Y && ty < DLIST_Y + DLIST_H) {
            int page = max(1, DVIS - 1);
            if (ty < DLIST_Y + DLIST_H / 2) s_scroll = max(0, s_scroll - page);
            else s_scroll += page;                          // clamped when drawn
            s_dirty = true;
        }
        return;
    }
    if (s_tab == TAB_SET) {
        if (ty < PAGE_Y + SET_SEG_H) {
            int p = tx / (SCREEN_W / 4);
            if (p > 3) p = 3;
            if (p != s_set_page) { s_set_page = p; s_dirty = true; }
            return;
        }
        if (s_set_page == 0) {
            if (in_rect(tx, ty, 164, PAGE_Y + SET_SEG_H + 4 + 3 * 42, 154, 38)) {     // scan balance
                g_ui_set.sched = (uint8_t)((g_ui_set.sched + 1) % SCHED_N);
                settings_save();
                if (s_cb.settings_changed) s_cb.settings_changed();
                s_dirty = true;
                return;
            }
            for (int i = 0; i < 7; i++) {
                int col = (i < 4) ? 0 : 1, row = (i < 4) ? i : (i - 4);
                if (!in_rect(tx, ty, col * 162 + 2, PAGE_Y + SET_SEG_H + 4 + row * 42, 158, 38)) continue;
                if (i < 5) {
                    g_ui_set.detect[i] = !g_ui_set.detect[i];
                    if (!g_ui_set.detect[i])                    // drop that type from the local view at once
                        for (int k = 0; k < g_model.ndets; k++)
                            if (g_model.dets[k].used && g_model.dets[k].type == i) g_model.dets[k].type = T_UNKNOWN;
                    settings_save();
                    if (s_cb.settings_changed) s_cb.settings_changed();
                } else if (i == 5) { g_ui_set.speaker = !g_ui_set.speaker; settings_save(); }
                else { g_ui_set.led = !g_ui_set.led; settings_save(); }
                s_dirty = true;
                return;
            }
            return;
        }
        if (s_set_page == 2) {                              // PROFILE: the open-network switch
            if (in_rect(tx, ty, 2, DEV_Y0, SCREEN_W - 4, 38)) {
                g_ui_set.opennet = !g_ui_set.opennet;
                if (s_cb.settings_changed) s_cb.settings_changed();
                s_dirty = true;
            }
            return;
        }
        if (s_set_page == 3) return;                        // ABOUT: nothing to tap
        int y = DEV_Y0 + DEV_ROW;                           // CHARGE MODE
        if (in_rect(tx, ty, 6, y, SCREEN_W - 12, 30)) { if (s_cb.enter_charge) s_cb.enter_charge(); return; }
        y += DEV_ROW;                                        // brightness - / +
        if (in_rect(tx, ty, 120, y, 40, 30)) { g_ui_set.brightness = (uint8_t)max(25, (int)g_ui_set.brightness - 30); apply_brightness(); s_dirty = true; return; }
        if (in_rect(tx, ty, SCREEN_W - 46, y, 40, 30)) { g_ui_set.brightness = (uint8_t)min(255, (int)g_ui_set.brightness + 30); apply_brightness(); s_dirty = true; return; }
        y += DEV_ROW;                                        // DIAGNOSTICS
        if (in_rect(tx, ty, 6, y, SCREEN_W - 12, 30)) { s_diag = true; status_ui_reset(); return; }
    }
}
