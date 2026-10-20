// SPDX-License-Identifier: GPL-3.0-or-later
#include "detections_api.h"
#include "esp_log.h"
#include "registry.h"
#include "radio_wifi.h"
#include "radio_ble.h"
#include <strings.h>
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
    radio_wifi_start(radio_on);     // brings up NVS, the event loop and the Wi-Fi driver first
    radio_ble_start(radio_on);
}

void det_radio_enable(bool on) { radio_wifi_set_enabled(on); radio_ble_set_enabled(on); }

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
bool det_encode_ble(ss_cbor_w_t* w) { return radio_ble_encode_devs(w); }

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

bool det_encode_profile(const char* mac_text, ss_cbor_w_t* w) {
    static Detection d;                           // link task only; keep off the stack
    bool found = false;
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    for (int i = 0; i < g_reg.count; i++) {
        char m[20]; g_reg.items[i].mac_str(m);
        if (strcasecmp(m, mac_text) == 0) { d = g_reg.items[i]; found = true; break; }
    }
    xSemaphoreGive(g_reg_mutex);
    if (!found) return false;

    uint32_t now = ms_now();
    const RemoteID& r = d.remote_id;
    bool rid = d.has_remote_id;
    // Count the keys first (CBOR maps carry their size up front)
    size_t pairs = 13;                            // always-present identity / timing keys below (mac .. age_ms)
    if (d.dev_name[0]) pairs++;
    if (d.company >= 0) pairs++;
    if (d.tx_power != TXP_NONE) pairs++;
    if (d.has_appearance) pairs++;
    if (d.apple_type != 0xFF) pairs += 2;
    if (d.svc_count) pairs++;
    if (d.ssid[0]) pairs++;
    if (d.probe_ssid[0]) pairs++;
    if (d.proto & PROTO_WIFI) pairs += 4;        // wifi_sec, wifi_std, beacon_int, hidden
    if (d.country[0]) pairs++;
    if (rid) pairs++;

    ss_cbor_map(w, pairs);
    char mac[20]; d.mac_str(mac);
    ss_cbor_kv_text(w, "mac", mac);
    ss_cbor_kv_text(w, "name", d.name);
    ss_cbor_kv_uint(w, "type", (uint32_t)d.type);
    ss_cbor_kv_uint(w, "conf", d.confidence);
    ss_cbor_kv_uint(w, "best", d.best_weight);
    ss_cbor_kv_bool(w, "corr", d.corroborated);
    ss_cbor_kv_uint(w, "atype", d.addr_type);
    ss_cbor_kv_text(w, "radio", radio_name(d));
    ss_cbor_kv_int(w, "rssi", d.rssi);
    ss_cbor_kv_int(w, "rmin", d.rssi_min);
    ss_cbor_kv_int(w, "rmax", d.rssi_max);
    ss_cbor_kv_uint(w, "seen", d.seen_count);
    ss_cbor_kv_uint(w, "age_ms", now - d.timestamp_ms);
    if (d.dev_name[0]) ss_cbor_kv_text(w, "dev_name", d.dev_name);
    if (d.company >= 0) ss_cbor_kv_uint(w, "company", (uint32_t)d.company);
    if (d.tx_power != TXP_NONE) ss_cbor_kv_int(w, "txp", d.tx_power);
    if (d.has_appearance) ss_cbor_kv_uint(w, "appear", d.appearance);
    if (d.apple_type != 0xFF) { ss_cbor_kv_uint(w, "apple", d.apple_type); ss_cbor_kv_uint(w, "apple_i", d.apple_info); }
    if (d.svc_count) {
        ss_cbor_text(w, "svc");
        ss_cbor_array(w, d.svc_count);
        for (int k = 0; k < d.svc_count; k++) ss_cbor_uint(w, d.svc[k]);
    }
    if (d.ssid[0]) ss_cbor_kv_text(w, "ssid", d.ssid);
    if (d.probe_ssid[0]) ss_cbor_kv_text(w, "probe", d.probe_ssid);
    if (d.proto & PROTO_WIFI) {
        ss_cbor_kv_uint(w, "wsec", d.wifi_sec);
        ss_cbor_kv_uint(w, "wstd", d.wifi_std);
        ss_cbor_kv_uint(w, "bint", d.beacon_int);
        ss_cbor_kv_bool(w, "hidden", d.hidden_ssid);
    }
    if (d.country[0]) ss_cbor_kv_text(w, "country", d.country);
    if (rid) {
        // Remote ID block. CBOR here has no floats, so positions are scaled integers and a key is
        // simply left out when the drone did not send that field.
        ss_cbor_text(w, "rid");
        size_t rp = 1 + (r.uas_id[0] ? 1 : 0) + (r.operator_id[0] ? 1 : 0);
        if (r.loc_valid) {
            rp += 2;                                                   // lat, lon
            if (r.height_agl > -1000) rp++;
            if (r.alt_geo > -1000) rp++;
            if (r.speed_ms < 254) rp++;
            if (r.vspeed_ms < 62) rp++;
            if (r.heading < 360) rp++;
        }
        if (r.op_valid) rp += 2;
        ss_cbor_map(w, rp);
        ss_cbor_kv_uint(w, "uat", r.ua_type);
        if (r.uas_id[0]) ss_cbor_kv_text(w, "uas", r.uas_id);
        if (r.operator_id[0]) ss_cbor_kv_text(w, "op", r.operator_id);
        if (r.loc_valid) {
            ss_cbor_kv_int(w, "lat", (int64_t)(r.lat * 1e6f));          // micro-degrees
            ss_cbor_kv_int(w, "lon", (int64_t)(r.lon * 1e6f));
            if (r.height_agl > -1000) ss_cbor_kv_int(w, "h_dm", (int64_t)(r.height_agl * 10.0f));   // decimetres AGL
            if (r.alt_geo > -1000)    ss_cbor_kv_int(w, "alt_dm", (int64_t)(r.alt_geo * 10.0f));    // decimetres, WGS84
            if (r.speed_ms < 254)     ss_cbor_kv_int(w, "spd_cms", (int64_t)(r.speed_ms * 100.0f)); // cm/s
            if (r.vspeed_ms < 62)     ss_cbor_kv_int(w, "vs_cms", (int64_t)(r.vspeed_ms * 100.0f));
            if (r.heading < 360)      ss_cbor_kv_int(w, "hdg", (int64_t)r.heading);                 // degrees
        }
        if (r.op_valid) {
            ss_cbor_kv_int(w, "olat", (int64_t)(r.op_lat * 1e6f));
            ss_cbor_kv_int(w, "olon", (int64_t)(r.op_lon * 1e6f));
        }
    }
    return !w->err;
}

void det_tick(void) {
    static uint32_t last_prune = 0, last_log = 0;
    uint32_t now = ms_now();
    if (now - last_prune >= 1000) { last_prune = now; registry_prune(); }
    // Stats line: every 5 s while scanning, every 60 s in charge mode (fewer wake-ups)
    uint32_t log_every = radio_wifi_enabled() ? 5000 : 60000;
    if (now - last_log >= log_every) { last_log = now; radio_wifi_log_stats(); radio_ble_log_stats(); }
}

}  // extern "C"
