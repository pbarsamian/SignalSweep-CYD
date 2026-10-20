// SPDX-License-Identifier: GPL-3.0-or-later
#include "registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "signatures.h"

static const char* TAG = "det";

DetectionRegistry g_reg;
SemaphoreHandle_t g_reg_mutex = nullptr;
QueueHandle_t     g_evq = nullptr;
ScanStats         g_stats = {};

uint32_t ms_now() { return (uint32_t)(esp_timer_get_time() / 1000); }

void registry_init() {
    g_reg_mutex = xSemaphoreCreateMutex();
    g_evq = xQueueCreate(24, sizeof(DetEvent));
}

// Ported from the standalone firmware's post_detection(); the alert queue became
// the event queue the link task drains.
void post_detection(Detection& d, int confidence, int bestWeight) {
    if (confidence > 100) confidence = 100;
    if (confidence < CONF_LIST_MIN) { STAT_INC(g_stats.post_low); return; }
    d.confidence  = (uint8_t)confidence;
    d.best_weight = (uint8_t)(bestWeight > 100 ? 100 : bestWeight);
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(5)) != pdTRUE) { STAT_INC(g_stats.post_lock); return; }
    int before = g_reg.count;
    int idx = g_reg.upsert(d);
    bool isNew = g_reg.count > before;
    bool alert = false;
    if (idx >= 0) {
        Detection& r = g_reg.items[idx];
        // Once per appearance: the flag clears when the entry is pruned.
        if (!r.alerted && r.best_weight >= CONF_ALERT_MIN && !r.muted) {
            r.alerted = true;
            alert = true;
        }
    }
    xSemaphoreGive(g_reg_mutex);
    if (isNew) {
        STAT_INC(g_stats.post_new);
        char m[20]; d.mac_str(m);
        ESP_LOGI(TAG, "NEW %s '%s' %s conf=%d best=%d rssi=%d ch=%d %s",
                 d.proto == PROTO_WIFI ? "WiFi" : "BLE", d.name, m, d.confidence, d.best_weight,
                 d.rssi, d.channel, alert ? "ALERT" : "listed");
    }
    if ((isNew || alert) && g_evq) {
        DetEvent ev; memcpy(ev.mac, d.mac, 6); ev.is_new = isNew; ev.alert = alert;
        xQueueSend(g_evq, &ev, 0);               // full queue: drop; the list still has it
    }
}

void registry_prune() {
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    g_reg.prune(ms_now());
    xSemaphoreGive(g_reg_mutex);
}
