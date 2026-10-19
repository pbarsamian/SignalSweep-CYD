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
#define SS_BATT_EN_GPIO  26
#define SS_BATT_ADC_GPIO 6

void     battery_init(void);
uint16_t battery_mv(void);   // cached for a few seconds; 0 = unavailable
