// SPDX-License-Identifier: GPL-3.0-or-later
// registry.h — the C5's detection table, scoring entry point and event queue.
// Scoring semantics are upstream's (B0z0dcl0wn/SignalSweep): LIST when the summed
// signals reach CONF_LIST_MIN, ALERT (one per appearance) when the strongest
// single signal reaches CONF_ALERT_MIN, entries go stale after 2 minutes.
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "detection.h"

struct ScanStats {
    volatile uint32_t wifi_mgmt, wifi_hit, wifi_drop;   // mgmt frames / rule matched / type off or unknown
    volatile uint32_t ble_adv, ble_hit, ble_drop;
    volatile uint32_t post_low, post_lock, post_new;    // below list threshold / mutex busy / new entries
};

struct DetEvent {            // queued from the radio callbacks, drained by the link task
    uint8_t mac[6];
    bool    is_new;
    bool    alert;
};

extern DetectionRegistry g_reg;          // guarded by g_reg_mutex
extern SemaphoreHandle_t g_reg_mutex;
extern QueueHandle_t     g_evq;          // DetEvent
extern ScanStats         g_stats;
// volatile counters: x++ is deprecated in C++20, so spell the increment out
#define STAT_INC(x) ((x) = (x) + 1)

void     registry_init();
uint32_t ms_now();
// Record a scored detection (called from radio callbacks: never blocks long).
void     post_detection(Detection& d, int confidence, int bestWeight);
void     registry_prune();               // drop entries older than DETECTION_STALE_MS
