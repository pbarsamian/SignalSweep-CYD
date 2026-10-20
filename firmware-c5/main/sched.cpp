// SPDX-License-Identifier: GPL-3.0-or-later
// sched.cpp — see sched.h.
#include "sched.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_wifi.h"
#include "radio_ble.h"
#include "registry.h"

static const char* TAG = "sched";

static volatile uint8_t  s_mode = SCHED_CONCURRENT;
static volatile uint8_t  s_bpct = 50;                     // = the scan window the BLE scan always used
static volatile uint16_t s_wms = 4000, s_bms = 2000;
static volatile uint8_t  s_lock = SCHED_LOCK_NONE;
static bool     s_phase_wifi = true;
static uint32_t s_phase_ms = 0;
static bool     s_want_wifi = true, s_want_ble = true;
static uint8_t  s_want_duty = 50;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void sched_set(int mode, int ble_pct, int wifi_ms, int ble_ms) {
    if (mode == SCHED_CONCURRENT || mode == SCHED_SLICED) s_mode = (uint8_t)mode;
    // 90 % at most: measured on hardware, a 100 % BLE window starves Wi-Fi completely (0 frames).
    // Blocking Wi-Fi on purpose is what SCHED_LOCK_BLE is for.
    if (ble_pct >= 0) s_bpct = (uint8_t)clampi(ble_pct, 10, 90);
    if (wifi_ms >= 0) s_wms = (uint16_t)clampi(wifi_ms, 500, 30000);
    if (ble_ms >= 0) s_bms = (uint16_t)clampi(ble_ms, 500, 30000);
    s_phase_ms = 0;                                       // restart the slice clock
    ESP_LOGI(TAG, "mode=%s ble_pct=%u slices wifi=%ums ble=%ums", s_mode == SCHED_SLICED ? "sliced" : "concurrent",
             (unsigned)s_bpct, (unsigned)s_wms, (unsigned)s_bms);
}

void sched_lock(uint8_t lock) {
    if (lock == s_lock) return;
    s_lock = lock;
    s_phase_ms = 0;
    ESP_LOGI(TAG, "lock=%s", lock == SCHED_LOCK_WIFI ? "wifi" : lock == SCHED_LOCK_BLE ? "ble" : "none");
}

static void tick(void) {
    uint32_t now = ms_now();
    bool wifi = true, ble = true;
    int duty = s_bpct;
    if (s_lock == SCHED_LOCK_WIFI) { ble = false; }
    else if (s_lock == SCHED_LOCK_BLE) { wifi = false; duty = 100; }
    else if (s_mode == SCHED_SLICED) {
        if (!s_phase_ms) { s_phase_ms = now; s_phase_wifi = true; }
        uint32_t len = s_phase_wifi ? s_wms : s_bms;
        if (now - s_phase_ms >= len) { s_phase_wifi = !s_phase_wifi; s_phase_ms = now; }
        wifi = s_phase_wifi; ble = !s_phase_wifi; duty = 100;
    }
    // Each call is a no-op when nothing changed; when the radios are off for charging they only record it.
    radio_wifi_set_slot_mode(s_lock == SCHED_LOCK_NONE && s_mode == SCHED_SLICED);
    radio_ble_set_duty(duty);
    radio_wifi_set_paused(!wifi);       // may pick the next band for a new slot
    radio_ble_set_paused(!ble);
    s_want_wifi = wifi; s_want_ble = ble; s_want_duty = (uint8_t)duty;   // published last, so the status never pairs a new radio with the old band
}

static void sched_task(void*) {
    for (;;) {
        tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void sched_start(void) {
    xTaskCreate(sched_task, "sched", 3072, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "started: concurrent, BLE window %u %%", (unsigned)s_bpct);
}

uint8_t sched_run_mask(void) { return (uint8_t)((s_want_wifi ? 1 : 0) | (s_want_ble ? 2 : 0)); }
uint8_t sched_lock_state(void) { return s_lock; }

void sched_log(void) {
    ESP_LOGI(TAG, "mode=%s ble_pct=%u slices %u/%u ms lock=%s now wifi=%s ble=%s duty=%u%%",
             s_mode == SCHED_SLICED ? "sliced" : "concurrent", (unsigned)s_bpct, (unsigned)s_wms, (unsigned)s_bms,
             s_lock == SCHED_LOCK_WIFI ? "wifi" : s_lock == SCHED_LOCK_BLE ? "ble" : "none",
             s_want_wifi ? "run" : "paused", s_want_ble ? "run" : "paused", (unsigned)s_want_duty);
}
