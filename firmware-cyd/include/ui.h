#pragma once
#include "detection.h"
#include "settings.h"

void ui_init(Settings& s);
void ui_update(const DetectionRegistry& reg, Settings& s);
void ui_update_scope(const uint8_t* buf, int len);
void ui_alert(const Detection& d, const Settings& s);
