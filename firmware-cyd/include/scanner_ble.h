#pragma once
#include "settings.h"
void scanner_ble_init(const Settings& s);
void scanner_ble_tick();
void scanner_ble_full_duty(bool full);   // focus: 100% scan window
