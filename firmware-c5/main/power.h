// SPDX-License-Identifier: GPL-3.0-or-later
// power.h — charge mode state machine (C5 side; the C5 measures the battery).
//
//   NORMAL --user request--> CHARGE_USER --user request--> NORMAL
//   NORMAL / CHARGE_USER --battery <= SS_BATT_LOW_MV--> CHARGE_FORCED
//   CHARGE_FORCED --battery > SS_BATT_EXIT_MV--> NORMAL     (no other way out)
//
// The 4.0 V hold applies ONLY to CHARGE_FORCED, i.e. a charge mode entered because
// of the low-voltage trip. A charge mode the user chose can be left at any time. In
// either charge state the radio is off.
// A reading of 0 means "no cell / unusable reading" and never forces charge mode.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ss_protocol.h"

void       power_init(uint16_t boot_mv);       // decide the boot state (before the radio starts)
ss_power_t power_state(void);
bool       power_radio_allowed(void);          // true only in SS_POWER_NORMAL
void       power_tick(uint16_t mv);            // ~1 Hz: forced entry/exit, applies radio on/off
ss_err_t   power_request(bool charge);         // user request; SS_ERR_LOCKED while forced

// The decision logic as pure functions, so power_selftest() can check it with no battery:
ss_power_t power_boot_state(uint16_t mv);
ss_power_t power_next_tick(ss_power_t s, uint16_t mv);
ss_err_t   power_next_request(ss_power_t s, bool charge, ss_power_t* out);
bool       power_selftest(void);               // logs one line per case; true if all pass
