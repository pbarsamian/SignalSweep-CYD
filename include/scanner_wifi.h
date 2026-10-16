#pragma once
#include "settings.h"
void scanner_wifi_init(const Settings& s);
void scanner_wifi_data_filter(bool on);                        // focus: mgmt+data vs mgmt only
void scanner_wifi_tick(uint8_t* out_scope, int lock_ch = 0);   // lock_ch > 0: focus, no hopping
