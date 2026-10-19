// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep C5 — the radio. M2: UART slave + dispatcher, canned scan data.
// Boot shadow is SILENT_RX (invalid until the CYD's HELLO), always.
#include "esp_log.h"
#include "battery.h"
#include "dispatch.h"
#include "uart_slave.h"

void app_main(void) {
    ESP_LOGI("main", "SignalSweep C5 M2 boot, shadow SILENT_RX");
    battery_init();
    dispatch_init();
    uart_slave_init();
    uart_slave_start();
}
