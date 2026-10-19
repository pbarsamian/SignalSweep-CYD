// SPDX-License-Identifier: GPL-3.0-or-later
// status_ui.h — M2 bring-up screen: mode pill, C5 link state, battery, counters.
// Direct TFT rendering (no sprites). The full detection UI is ported in M4/M5.
#pragma once
#include <Arduino.h>
#include "ss_protocol.h"

struct C5Status {
    bool     link_up = false;
    char     fw[20] = {};
    bool     have = false;           // at least one GET_STATUS answered
    uint8_t  mode = 0;               // C5's shadow
    bool     shadow_valid = false;
    uint32_t up_ms = 0;
    uint16_t batt_mv = 0;            // 0 = unavailable
    bool     batt_low = false;
};

struct StatusView {
    const C5Status* c5;
    int   selftest_pass, selftest_total;
    uint32_t tx, rx, nack, crc_err, timeouts, rtt_ms, raw;
    char  last[48];                  // last notable result (dev console / errors)
};

void status_ui_init();                         // display + backlight, boot splash
void status_ui_draw(const StatusView& v, ss_mode_t mode);   // cheap: redraws changed lines only
