// SPDX-License-Identifier: GPL-3.0-or-later
// keyboard.cpp — popup on-screen keyboard (issue #24). Two pages (letters, symbols), one-shot shift,
// DEL, space, Cancel / OK, and a SHOW toggle for secret fields. Layout is 320x240 landscape:
//   y 0..17    title bar (title, length, SHOW)
//   y 20..45   text field
//   y 48..237  five rows of keys, 38 px each, widths in each row sum to 320
#include "keyboard.h"
#include "theme.h"
#include <ctype.h>

extern TFT_eSPI tft;                         // owned by status_ui.cpp

enum Act : uint8_t { A_CHAR, A_SHIFT, A_BKSP, A_PAGE, A_SPACE, A_CANCEL, A_OK };

struct Key { const char* lab; uint8_t w; uint8_t act; char ch; };   // lab NULL: the label is ch (shift applies on the letters page)

#define K(c)      { NULL, 32, A_CHAR, c }
#define KW(c, w)  { NULL, w,  A_CHAR, c }

static const int KROWS = 5, KY0 = 48, KH = 38;

static const Key P_LET[KROWS][11] = {
    { K('1'), K('2'), K('3'), K('4'), K('5'), K('6'), K('7'), K('8'), K('9'), K('0') },
    { K('q'), K('w'), K('e'), K('r'), K('t'), K('y'), K('u'), K('i'), K('o'), K('p') },
    { K('a'), K('s'), K('d'), K('f'), K('g'), K('h'), K('j'), K('k'), K('l'), { "DEL", 32, A_BKSP, 0 } },
    { { "SHIFT", 48, A_SHIFT, 0 }, K('z'), K('x'), K('c'), K('v'), K('b'), K('n'), K('m'), KW('.', 48) },
    { { "?123", 56, A_PAGE, 0 }, { "space", 112, A_SPACE, ' ' }, { "Cancel", 76, A_CANCEL, 0 }, { "OK", 76, A_OK, 0 } },
};
static const Key P_SYM[KROWS][11] = {
    { K('!'), K('@'), K('#'), K('$'), K('%'), K('^'), K('&'), K('*'), K('('), K(')') },
    { K('-'), K('_'), K('='), K('+'), K('['), K(']'), K('{'), K('}'), K('~'), K('`') },
    { K('\\'), K('|'), K(';'), K(':'), K('\''), K('"'), K(','), K('.'), K('/'), { "DEL", 32, A_BKSP, 0 } },
    { KW('<', 107), KW('>', 107), KW('?', 106) },
    { { "abc", 56, A_PAGE, 0 }, { "space", 112, A_SPACE, ' ' }, { "Cancel", 76, A_CANCEL, 0 }, { "OK", 76, A_OK, 0 } },
};

static bool     s_active = false;
static char     s_title[24];
static char     s_buf[KBD_MAX_LEN + 1];
static size_t   s_len = 0, s_max = KBD_MAX_LEN;
static bool     s_secret = false, s_show = false;
static bool     s_symbols = false, s_shift = false;
static KbdDone  s_done = nullptr;
static void*    s_ctx = nullptr;
static bool     s_full = true;               // repaint everything (first draw, page change)
static bool     s_dirty_field = true, s_dirty_keys = true;
static int      s_flash_r = -1, s_flash_i = -1;
static uint32_t s_flash_until = 0;

static const Key (*page())[11] { return s_symbols ? P_SYM : P_LET; }

static int row_count(const Key* row) { int n = 0; while (n < 11 && row[n].w) n++; return n; }

static char key_char(const Key& k) {
    return (!s_symbols && s_shift && isalpha((unsigned char)k.ch)) ? (char)toupper((unsigned char)k.ch) : k.ch;
}

static void key_rect(int r, int i, int* x, int* y, int* w, int* h) {
    const Key* row = page()[r];
    int xx = 0;
    for (int j = 0; j < i; j++) xx += row[j].w;
    *x = xx; *y = KY0 + r * KH; *w = row[i].w; *h = KH;
}

static void draw_key(int r, int i) {
    const Key& k = page()[r][i];
    int x, y, w, h;
    key_rect(r, i, &x, &y, &w, &h);
    bool flash = (r == s_flash_r && i == s_flash_i);
    bool on = (k.act == A_SHIFT && s_shift);
    uint16_t bg = flash ? C_ACCENT : on ? C_SIGNAL2 : (k.act == A_OK ? C_SIGNAL2 : C_PANEL);
    uint16_t fg = flash ? C_BLACK : (k.act == A_CANCEL ? C_WARN : C_WHITE);
    tft.fillRect(x + 1, y + 1, w - 2, h - 2, bg);
    tft.drawRect(x + 1, y + 1, w - 2, h - 2, C_BORDER);
    tft.setTextColor(fg, bg);
    tft.setTextDatum(MC_DATUM);
    if (k.lab) tft.drawString(k.lab, x + w / 2, y + h / 2, strlen(k.lab) > 3 ? 1 : 2);
    else { char s[2] = { key_char(k), 0 }; tft.drawString(s, x + w / 2, y + h / 2, 2); }
}

static void draw_title() {
    tft.fillRect(0, 0, SCREEN_W, 18, C_PANEL2);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_ACCENT, C_PANEL2);
    tft.drawString(s_title, 4, 9, 2);
    char n[12];
    snprintf(n, sizeof(n), "%u/%u", (unsigned)s_len, (unsigned)s_max);
    tft.setTextColor(C_MDGREY, C_PANEL2);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(n, s_secret ? SCREEN_W - 64 : SCREEN_W - 4, 9, 1);
    if (s_secret) {
        tft.fillRect(SCREEN_W - 58, 1, 56, 16, s_show ? C_SIGNAL2 : C_PANEL);
        tft.drawRect(SCREEN_W - 58, 1, 56, 16, C_BORDER);
        tft.setTextColor(C_WHITE, s_show ? C_SIGNAL2 : C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(s_show ? "HIDE" : "SHOW", SCREEN_W - 30, 9, 1);
    }
}

static void draw_field() {
    tft.fillRect(0, 20, SCREEN_W, 26, C_BLACK);
    tft.drawRect(2, 20, SCREEN_W - 4, 26, C_BORDER);
    char shown[KBD_MAX_LEN + 2];
    size_t n = s_len;
    if (s_secret && !s_show) { memset(shown, '*', n); shown[n] = 0; }
    else memcpy(shown, s_buf, n + 1);
    const char* p = shown;
    char tail[KBD_MAX_LEN + 2];
    if (tft.textWidth(p, 2) > SCREEN_W - 28) {            // too long: keep the end, mark the cut with "<"
        size_t skip = 1;
        while (skip < n) {
            snprintf(tail, sizeof(tail), "<%s", shown + skip);
            if (tft.textWidth(tail, 2) <= SCREEN_W - 28) break;
            skip++;
        }
        p = tail;
    }
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    int w = tft.drawString(p, 8, 33, 2);
    tft.fillRect(8 + w + 1, 25, 2, 16, C_ACCENT);         // cursor: input only appends
}

void kbd_open(const char* title, const char* initial, size_t maxlen, bool secret, KbdDone done, void* ctx) {
    strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = 0;
    s_max = maxlen < 1 ? 1 : (maxlen > KBD_MAX_LEN ? KBD_MAX_LEN : maxlen);
    memset(s_buf, 0, sizeof(s_buf));
    if (initial) strncpy(s_buf, initial, s_max);
    s_len = strlen(s_buf);
    s_secret = secret; s_show = false;
    s_symbols = false; s_shift = false;
    s_done = done; s_ctx = ctx;
    s_flash_r = s_flash_i = -1;
    s_full = s_dirty_field = s_dirty_keys = true;
    s_active = true;
}

bool kbd_active(void) { return s_active; }

static void finish(bool ok) {
    KbdDone cb = s_done; void* ctx = s_ctx;
    s_active = false; s_done = nullptr; s_ctx = nullptr;
    char text[KBD_MAX_LEN + 1] = {};                       // the callback gets a copy, so it may open the next keyboard
    if (ok) memcpy(text, s_buf, sizeof(text));            // (kbd_open() inside the callback reuses s_buf)
    memset(s_buf, 0, sizeof(s_buf));                       // do not leave a password lying in RAM
    s_len = 0;
    if (cb) cb(ok, text, ctx);
    memset(text, 0, sizeof(text));
}

void kbd_touch(int tx, int ty) {
    if (!s_active) return;
    if (ty < 20) {                                         // title bar: the SHOW / HIDE toggle
        if (s_secret && tx >= SCREEN_W - 60) { s_show = !s_show; s_dirty_field = true; s_dirty_keys = true; s_full = false; }
        return;
    }
    if (ty < KY0) return;
    int r = (ty - KY0) / KH;
    if (r >= KROWS) return;
    const Key* row = page()[r];
    int n = row_count(row), xx = 0, hit = -1;
    for (int i = 0; i < n; i++) { if (tx >= xx && tx < xx + row[i].w) { hit = i; break; } xx += row[i].w; }
    if (hit < 0) return;                                   // a row narrower than the screen leaves a gap
    const Key& k = row[hit];
    s_flash_r = r; s_flash_i = hit; s_flash_until = millis() + 100;
    s_dirty_keys = true;
    switch (k.act) {
        case A_CHAR:
        case A_SPACE:
            if (s_len < s_max) { s_buf[s_len++] = key_char(k); s_buf[s_len] = 0; s_dirty_field = true; }
            s_shift = false;
            break;
        case A_BKSP:
            if (s_len) { s_buf[--s_len] = 0; s_dirty_field = true; }
            break;
        case A_SHIFT: s_shift = !s_shift; break;
        case A_PAGE:  s_symbols = !s_symbols; s_shift = false; s_flash_r = s_flash_i = -1; s_full = true; break;
        case A_CANCEL: finish(false); return;
        case A_OK:     finish(true);  return;
    }
}

void kbd_draw(void) {
    if (!s_active) return;
    if (s_flash_r >= 0 && (int32_t)(millis() - s_flash_until) >= 0) { s_flash_r = s_flash_i = -1; s_dirty_keys = true; }
    if (!(s_full || s_dirty_field || s_dirty_keys)) return;
    if (s_full) tft.fillScreen(C_BLACK);
    if (s_full || s_dirty_field) { draw_title(); draw_field(); }
    if (s_full || s_dirty_keys)
        for (int r = 0; r < KROWS; r++) { const Key* row = page()[r]; for (int i = 0, n = row_count(row); i < n; i++) draw_key(r, i); }
    s_full = s_dirty_field = s_dirty_keys = false;
}
