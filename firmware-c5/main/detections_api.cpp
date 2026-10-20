// SPDX-License-Identifier: GPL-3.0-or-later
#include "detections_api.h"
#include "esp_log.h"
#include "registry.h"
#include "radio_wifi.h"
#include "signatures.h"

#define LIST_PAGE 10                 // entries per LIST_DETECTIONS reply (keeps it well under one frame)

static const char* radio_name(const Detection& d) {
    if ((d.proto & PROTO_WIFI) && (d.proto & PROTO_BLE)) return "both";
    return (d.proto & PROTO_BLE) ? "ble" : "wifi";
}

// One detection as a CBOR map. `extra` adds the event flags ("new", "alert").
static void encode_det(ss_cbor_w_t* w, const Detection& d, uint32_t now, const DetEvent* ev) {
    char mac[20]; d.mac_str(mac);
    size_t pairs = 11 + (d.ssid[0] ? 1 : 0) + (ev ? 2 : 0);
    ss_cbor_map(w, pairs);
    ss_cbor_kv_text(w, "mac", mac);
    ss_cbor_kv_text(w, "name", d.name);
    ss_cbor_kv_uint(w, "type", (uint32_t)d.type);
    ss_cbor_kv_uint(w, "conf", d.confidence);
    ss_cbor_kv_uint(w, "best", d.best_weight);
    ss_cbor_kv_int(w, "rssi", d.rssi);
    ss_cbor_kv_uint(w, "ch", (uint32_t)d.channel);
    ss_cbor_kv_uint(w, "band", d.band);
    ss_cbor_kv_text(w, "radio", radio_name(d));
    ss_cbor_kv_uint(w, "age_ms", now - d.timestamp_ms);
    ss_cbor_kv_uint(w, "seen", d.seen_count);
    if (d.ssid[0]) ss_cbor_kv_text(w, "ssid", d.ssid);
    if (ev) {
        ss_cbor_kv_bool(w, "new", ev->is_new);
        ss_cbor_kv_bool(w, "alert", ev->alert);
    }
}

extern "C" {

void det_api_start(bool radio_on) {
    registry_init();
    radio_wifi_start(radio_on);
}

void det_radio_enable(bool on) { radio_wifi_set_enabled(on); }

void det_set_config(int drone, int tracker, int camera, int bodycam, int attack) {
    if (drone   >= 0) g_cfg.detect_drone   = drone;
    if (tracker >= 0) g_cfg.detect_tracker = tracker;
    if (camera  >= 0) g_cfg.detect_camera  = camera;
    if (bodycam >= 0) g_cfg.detect_bodycam = bodycam;
    if (attack  >= 0) g_cfg.detect_attack  = attack;
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (int i = 0; i < g_reg.count; ) {                       // purge any type that is now off
        if (!type_enabled(g_reg.items[i].type, g_cfg)) {
            for (int j = i; j < g_reg.count - 1; j++) g_reg.items[j] = g_reg.items[j + 1];
            g_reg.count--;
        } else i++;
    }
    xSemaphoreGive(g_reg_mutex);
    ESP_LOGI("cfg", "detect drone=%d tracker=%d camera=%d bodycam=%d attack=%d", g_cfg.detect_drone,
             g_cfg.detect_tracker, g_cfg.detect_camera, g_cfg.detect_bodycam, g_cfg.detect_attack);
}

uint32_t det_count(void) { return (uint32_t)g_reg.count; }

bool det_encode_list(ss_cbor_w_t* w, uint32_t offset) {
    static Detection snap[MAX_DETECTIONS];   // link task only; keep off the stack
    int n = 0;
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    n = g_reg.count;
    for (int i = 0; i < n; i++) snap[i] = g_reg.items[i];
    xSemaphoreGive(g_reg_mutex);

    uint32_t now = ms_now();
    uint32_t start = offset < (uint32_t)n ? offset : (uint32_t)n;
    uint32_t cnt = (uint32_t)n - start;
    if (cnt > LIST_PAGE) cnt = LIST_PAGE;
    ss_cbor_map(w, 3);
    ss_cbor_kv_uint(w, "total", (uint32_t)n);
    ss_cbor_kv_uint(w, "offset", start);
    ss_cbor_text(w, "items");
    ss_cbor_array(w, cnt);
    for (uint32_t i = 0; i < cnt; i++) encode_det(w, snap[start + i], now, nullptr);
    return !w->err;
}

bool det_encode_aps(ss_cbor_w_t* w) { return radio_wifi_encode_aps(w); }

bool det_pop_event(ss_cbor_w_t* w) {
    DetEvent ev;
    if (!g_evq || xQueueReceive(g_evq, &ev, 0) != pdTRUE) return false;
    static Detection d;
    bool found = false;
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < g_reg.count; i++)
            if (memcmp(g_reg.items[i].mac, ev.mac, 6) == 0) { d = g_reg.items[i]; found = true; break; }
        xSemaphoreGive(g_reg_mutex);
    }
    if (!found) return false;                // pruned before we got to it
    encode_det(w, d, ms_now(), &ev);
    return !w->err;
}

void det_tick(void) {
    static uint32_t last_prune = 0, last_log = 0;
    uint32_t now = ms_now();
    if (now - last_prune >= 1000) { last_prune = now; registry_prune(); }
    // Stats line: every 5 s while scanning, every 60 s in charge mode (fewer wake-ups)
    uint32_t log_every = radio_wifi_enabled() ? 5000 : 60000;
    if (now - last_log >= log_every) { last_log = now; radio_wifi_log_stats(); }
}

}  // extern "C"
