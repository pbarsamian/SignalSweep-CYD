// SPDX-License-Identifier: GPL-3.0-or-later
// ui.h — the CYD's normal screens: header, tab bar, SCOPE / LIST / INFO / SET pages.
// The look is the standalone firmware's (dark oscilloscope theme). Charge mode keeps its own
// screens (status_ui.*); the old bring-up screen survives as SET > DEVICE > DIAGNOSTICS.
#pragma once
#include <Arduino.h>
#include "ss_protocol.h"
#include "status_ui.h"

enum UiTab { TAB_SCOPE = 0, TAB_LIST = 1, TAB_INFO = 2, TAB_SET = 3 };

struct UiSettings {
    bool    detect[5] = { true, true, true, true, true };   // drone, tracker, camera, bodycam, attack
    bool    speaker = true;
    bool    led = true;
    uint8_t brightness = 200;                                // backlight PWM 25..255
    uint8_t sched = 1;                                       // scan balance preset (SCHED_PRESETS index)
};
extern UiSettings g_ui_set;

// How the C5 splits its one radio between Wi-Fi and BLE (see firmware-c5/main/sched.h). The C5 takes the raw
// numbers in SET_CONFIG {smode, bpct, wms, bms}; the names live here.
struct SchedPreset { const char* name; uint8_t mode; uint8_t bpct; uint16_t wms, bms, bdms; };
extern const SchedPreset SCHED_PRESETS[];
extern const int SCHED_N;

struct UiCallbacks {
    void (*enter_charge)();          // user pressed CHARGE MODE
    void (*settings_changed)();      // detection toggles changed: push them to the C5
};

struct UiCtx {
    const C5Status*   c5;
    ss_mode_t         mode;
    const StatusView* diag;          // for the diagnostics page
};

void ui_init(const UiCallbacks& cb);          // after status_ui_init(): loads saved settings
void ui_update(const UiCtx& ctx);             // call every loop (not while in charge mode)
void ui_touch(int x, int y);                  // a touch-down edge, in normal mode
void ui_alert(uint8_t type);                  // beep + LED for a new alert (respects settings)
int  ui_active_tab();                         // the main loop polls the C5 based on this
void ui_invalidate();                         // redraw everything (e.g. returning from charge mode)
bool ui_wants_aps();                          // true while the SCOPE tab shows a Wi-Fi band
bool ui_wants_ble();                          // true while the SCOPE tab shows the BLE view
bool ui_wants_focus(char* mac_out);            // true while INFO shows a live detection: the C5 should park on it
int  ui_focus_view_period();                   // ms between focus-view polls for the page showing (0 = none)
bool ui_wants_profile(char* mac_out);         // true while INFO shows a detection; copies its MAC text
