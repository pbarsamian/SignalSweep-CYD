// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep C5 — the radio. M2: UART slave + dispatcher, canned scan data.
// Boot shadow is SILENT_RX (invalid until the CYD's HELLO), always.
#include "esp_log.h"
#include "battery.h"
#include "dispatch.h"
#include "uart_slave.h"
#include "detections_api.h"
#include "power.h"

void app_main(void) {
    ESP_LOGI("main", "SignalSweep C5 M3a boot, shadow SILENT_RX");
    battery_init();
    power_init(battery_mv());                     // a low pack boots straight into forced charge mode
    dispatch_init();
    uart_slave_init();
    uart_slave_start();
    det_api_start(power_radio_allowed());         // passive dual-band Wi-Fi capture (SILENT_RX-safe)
}
