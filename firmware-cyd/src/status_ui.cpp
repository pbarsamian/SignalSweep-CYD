// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_ui.h"
#include <TFT_eSPI.h>
#include "board_pins.h"
#include "mode.h"

TFT_eSPI tft;                       // shared with ui.cpp
uint8_t   g_backlight = 200;         // normal backlight PWM (settings page changes it)

#define C_BG     TFT_BLACK
#define C_FG     TFT_WHITE
#define C_DIM    0x7BCF
#define C_OK     TFT_GREEN
#define C_WARN   0xFD20
#define C_BAD    TFT_RED
#define C_ACCENT TFT_CYAN

void status_ui_init() {
    tft.init();
    tft.setRotation(3);
    // Panel colour fix (confirmed on this board, MODE 2): MADCTL 0xE8 + inversion ON.
    tft.writecommand(0x36);
    tft.writedata(0xE8);
    tft.invertDisplay(true);
    tft.fillScreen(C_BG);

    // Backlight PWM goes AFTER tft.init(): TFT_eSPI's init() does pinMode(TFT_BL) + digitalWrite(HIGH),
    // which detaches the pin from any LEDC channel set up earlier (so dimming and screen-off silently
    // did nothing). Attaching here takes the pin back.
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_TFT_BL, 0);
    ledcWrite(0, g_backlight);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_OK, C_BG);
    tft.drawString("SignalSweep split", SCREEN_W / 2, 100, 4);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("control plane M2 - starting", SCREEN_W / 2, 135, 2);
}

// One text line that only repaints when its contents change
struct Line { char last[64]; uint16_t lastcol; };
static Line L[12];

static void line(int idx, int y, const char* text, uint16_t col, int font = 2) {
    if (strncmp(L[idx].last, text, sizeof(L[idx].last)) == 0 && L[idx].lastcol == col) return;
    strncpy(L[idx].last, text, sizeof(L[idx].last) - 1);
    L[idx].last[sizeof(L[idx].last) - 1] = 0;
    L[idx].lastcol = col;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(col, C_BG);
    tft.fillRect(0, y, SCREEN_W, font == 4 ? 26 : 18, C_BG);
    tft.drawString(text, 6, y, font);
}

static bool s_first = true;
static ss_mode_t s_pill_shown = (ss_mode_t)99;
#define CHG_BTN_X 214
#define CHG_BTN_Y 28
#define CHG_BTN_W 100
#define CHG_BTN_H 30

void status_ui_draw(const StatusView& v, ss_mode_t mode) {
    if (s_first) {
        tft.fillScreen(C_BG);
        memset(L, 0, sizeof(L));
        s_first = false;
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(C_ACCENT, C_BG);
        tft.drawString("SignalSweep split  M2", 6, 4, 2);
        tft.drawFastHLine(0, 22, SCREEN_W, C_DIM);
    }
    char b[64];

    // Mode pill (drawn from the single authoritative mode value)
    if (mode != s_pill_shown) {
        s_pill_shown = mode;
        uint16_t pc = (mode == SS_MODE_SILENT_RX) ? C_OK : C_WARN;
        tft.fillRoundRect(6, 28, 200, 30, 15, pc);
        tft.setTextColor(TFT_BLACK, pc);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(mode_name(mode), 106, 44, 4);
        tft.fillRoundRect(CHG_BTN_X, CHG_BTN_Y, CHG_BTN_W, CHG_BTN_H, 8, C_DIM);
        tft.setTextColor(TFT_BLACK, C_DIM);
        tft.drawString("CHARGE", CHG_BTN_X + CHG_BTN_W / 2, CHG_BTN_Y + CHG_BTN_H / 2, 2);
    }

    const C5Status& c = *v.c5;
    snprintf(b, sizeof(b), "C5 link: %s", c.link_up ? "UP" : "NO LINK");
    line(0, 66, b, c.link_up ? C_OK : C_BAD);

    if (c.link_up && c.fw[0]) snprintf(b, sizeof(b), "fw %s  up %us", c.fw, (unsigned)(c.up_ms / 1000));
    else snprintf(b, sizeof(b), "waiting for HELLO...");
    line(1, 84, b, c.link_up ? C_FG : C_DIM);

    if (c.link_up && c.have) {
        snprintf(b, sizeof(b), "C5 shadow: %s%s", mode_name((ss_mode_t)c.mode),
                 c.shadow_valid ? "" : " (INVALID)");
        line(2, 102, b, (c.shadow_valid && c.mode == (uint8_t)mode) ? C_FG : C_BAD);
    } else line(2, 102, "C5 shadow: --", C_DIM);

    if (c.link_up && c.batt_mv) {
        snprintf(b, sizeof(b), "Battery: %u.%02u V%s", c.batt_mv / 1000, (c.batt_mv % 1000) / 10,
                 c.batt_low ? "  LOW" : "");
        line(3, 120, b, c.batt_low ? C_BAD : C_FG);
    } else line(3, 120, "Battery: n/a", C_DIM);

    snprintf(b, sizeof(b), "tx %u rx %u raw %u rtt %ums", (unsigned)v.tx, (unsigned)v.rx, (unsigned)v.raw, (unsigned)v.rtt_ms);
    line(4, 144, b, C_FG);
    snprintf(b, sizeof(b), "nack %u  crc %u  timeout %u", (unsigned)v.nack, (unsigned)v.crc_err,
             (unsigned)v.timeouts);
    line(5, 162, b, (v.nack || v.crc_err || v.timeouts) ? C_WARN : C_FG);

    snprintf(b, sizeof(b), "selftest %d/%d %s", v.selftest_pass, v.selftest_total,
             v.selftest_pass == v.selftest_total ? "PASS" : "FAIL");
    line(6, 186, b, v.selftest_pass == v.selftest_total ? C_OK : C_BAD);

    line(7, 208, v.last[0] ? v.last : "last: -", C_ACCENT, 2);
}

// Repaint the diagnostics page from scratch (it is an overlay now)
void status_ui_reset() {
    s_first = true;
    s_pill_shown = (ss_mode_t)99;
    memset(L, 0, sizeof(L));
}

bool status_ui_charge_button_hit(int x, int y) {
    return x >= CHG_BTN_X && x < CHG_BTN_X + CHG_BTN_W && y >= CHG_BTN_Y && y < CHG_BTN_Y + CHG_BTN_H;
}

// ---- charge mode screen ---------------------------------------------------------
// Everything except the battery voltage is switched off: backlight dimmed, CPU slowed to
// 80 MHz (the lowest speed that keeps the APB clock, and so the UART baud rate, intact),
// and the screen redraws only when a value changes.
static char s_chg_last[4][44];
static uint16_t s_chg_col[4];

static void chg_text(int idx, int y, const char* text, uint16_t col, int font) {
    if (strncmp(s_chg_last[idx], text, sizeof(s_chg_last[idx])) == 0 && s_chg_col[idx] == col) return;
    strncpy(s_chg_last[idx], text, sizeof(s_chg_last[idx]) - 1);
    s_chg_last[idx][sizeof(s_chg_last[idx]) - 1] = 0;
    s_chg_col[idx] = col;
    tft.fillRect(0, y - (font == 7 ? 8 : 4), SCREEN_W, font == 7 ? 62 : 26, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_BG);
    tft.drawString(text, SCREEN_W / 2, y + (font == 7 ? 24 : 10), font);
}

void status_ui_set_charge(bool on) {
    status_ui_screen_power(true);               // always start (and leave) with the panel awake
    memset(s_chg_last, 0, sizeof(s_chg_last));
    memset(s_chg_col, 0, sizeof(s_chg_col));
    tft.fillScreen(C_BG);
    if (on) {
        ledcWrite(0, 25);                 // backlight to about 10%
        setCpuFrequencyMhz(80);
    } else {
        ledcWrite(0, g_backlight);
        setCpuFrequencyMhz(240);
        s_first = true;                   // normal screen repaints from scratch
        s_pill_shown = (ss_mode_t)99;
        memset(L, 0, sizeof(L));
    }
}

void status_ui_draw_charge(const C5Status& c, uint32_t cut_left_s) {
    bool forced = (c.power == SS_POWER_CHARGE_FORCED);
    chg_text(0, 20, forced ? "LOW BATTERY - CHARGE REQUIRED" : "CHARGE MODE", forced ? C_BAD : C_ACCENT, 2);
    char b[16];
    if (c.link_up && c.batt_mv) {
        snprintf(b, sizeof(b), "%u.%02u", c.batt_mv / 1000, (c.batt_mv % 1000) / 10);
        chg_text(1, 70, b, forced ? C_WARN : C_OK, 7);
    } else {
        chg_text(1, 70, c.link_up ? "n/a" : "no link", C_DIM, 4);
    }
    chg_text(2, 170, forced ? "locked until over 4.00 V" : "tap anywhere to exit", C_DIM, 2);
    if (cut_left_s) {
        char t[44];
        snprintf(t, sizeof(t), "CYD power off in %us%s", (unsigned)cut_left_s, forced ? "" : "  (BOOT on C5 wakes)");
        chg_text(3, 200, t, C_WARN, 2);
    } else {
        chg_text(3, 200, "", C_DIM, 2);
    }
}

// Screen off = backlight to zero. The ILI9341 itself is deliberately NOT put into sleep
// (SLPIN/DISPOFF): the panel shows white in sleep mode on many boards, and the backlight is
// where the power goes, so the sleep command would save almost nothing for that risk.
static bool s_panel_on = true;
void status_ui_screen_power(bool on) {
    if (on == s_panel_on) return;
    s_panel_on = on;
    if (on) {
        memset(s_chg_last, 0, sizeof(s_chg_last));   // force a repaint of every line
        memset(s_chg_col, 0, sizeof(s_chg_col));
        tft.fillScreen(C_BG);
        ledcWrite(0, 25);
    } else {
        ledcWrite(0, 0);
    }
}
