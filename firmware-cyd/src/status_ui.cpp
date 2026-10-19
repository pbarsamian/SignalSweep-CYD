// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_ui.h"
#include <TFT_eSPI.h>
#include "board_pins.h"
#include "mode.h"

static TFT_eSPI tft;

#define C_BG     TFT_BLACK
#define C_FG     TFT_WHITE
#define C_DIM    0x7BCF
#define C_OK     TFT_GREEN
#define C_WARN   0xFD20
#define C_BAD    TFT_RED
#define C_ACCENT TFT_CYAN

void status_ui_init() {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_TFT_BL, 0);
    ledcWrite(0, 200);

    tft.init();
    tft.setRotation(3);
    // Panel colour fix (confirmed on this board, MODE 2): MADCTL 0xE8 + inversion ON.
    tft.writecommand(0x36);
    tft.writedata(0xE8);
    tft.invertDisplay(true);
    tft.fillScreen(C_BG);

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
    static ss_mode_t shown = (ss_mode_t)99;
    if (mode != shown) {
        shown = mode;
        uint16_t pc = (mode == SS_MODE_SILENT_RX) ? C_OK : C_WARN;
        tft.fillRoundRect(6, 28, 200, 30, 15, pc);
        tft.setTextColor(TFT_BLACK, pc);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(mode_name(mode), 106, 44, 4);
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
