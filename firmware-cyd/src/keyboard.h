// SPDX-License-Identifier: GPL-3.0-or-later
// keyboard.h — a popup on-screen keyboard for text entry (issue #24). Full-screen overlay, direct TFT rendering.
//
// The caller opens it with kbd_open() and gets the result through the callback when the user taps OK or Cancel.
// While it is open, ui.cpp routes touches to kbd_touch() and calls kbd_draw() instead of drawing the pages.
// The typed text is never printed to serial or written to the SD card, and the buffer is wiped after the callback.
#pragma once
#include <Arduino.h>

#define KBD_MAX_LEN 64                       // WPA2 passphrases go up to 63 characters

// ok = true: text is what was typed (may be empty). ok = false: cancelled, text is "".
// text is only valid during the call: copy it if you need it.
typedef void (*KbdDone)(bool ok, const char* text, void* ctx);

// title: shown in the top bar. initial: pre-filled text (may be NULL). maxlen: 1..KBD_MAX_LEN.
// secret: show * instead of the text, with a SHOW toggle in the top bar.
void kbd_open(const char* title, const char* initial, size_t maxlen, bool secret, KbdDone done, void* ctx);
bool kbd_active(void);
void kbd_touch(int tx, int ty);              // a touch-down edge while the keyboard is open
void kbd_draw(void);                         // call every loop while open: repaints only what changed
