// SPDX-License-Identifier: GPL-3.0-or-later
// battery.h — VBAT read on the XIAO ESP32-C5.
//
// UNVERIFIED PIN ROLES: from Seeed's schematic Rev V1.1 the divider (2 x 100K,
// "Battery voltage = ADC sampling voltage * 2.0") is switched by GPIO26
// (net GPIO26_ADC_CRL) and read on GPIO6 / ADC1_CH5. Check against Seeed's own
// example before trusting the number; set SS_BATT_SENSE to 0 to disable.
#pragma once
#include <stdint.h>

#ifndef SS_BATT_SENSE
#define SS_BATT_SENSE 1
#endif
// Readings below this are not a usable single LiPo cell. With no cell attached the
// sense line floated at about 2.3 V on USB power (observed 2026-10-19), so anything
// under the floor is reported as unavailable (0) instead of a misleading voltage.
#define SS_BATT_MIN_VALID_MV 2500

// Test builds only: SS_BATT_TEST_SWEEP=1 replaces the reading for the first minute after boot:
// 3300 mV for 30 s (forces charge mode), then ramps to 4100 mV over 20 s (exercises the 4.0 V
// exit). Lets the forced path be run end to end without draining a real cell. Default 0.
#ifndef SS_BATT_TEST_SWEEP
#define SS_BATT_TEST_SWEEP 0
#endif

#define SS_BATT_EN_GPIO  26
#define SS_BATT_ADC_GPIO 6

void     battery_init(void);
uint16_t battery_mv(void);   // cached for a few seconds; 0 = unavailable
