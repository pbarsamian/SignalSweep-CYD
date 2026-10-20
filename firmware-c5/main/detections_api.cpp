// SPDX-License-Identifier: GPL-3.0-or-later
#include "detections_api.h"
#include "esp_log.h"
#include "registry.h"
#include "radio_wifi.h"
#include "radio_ble.h"
#include <strings.h>
#include "signatures.h"
#include "focus.h"
#include "ss_protocol.h"
#include <stdio.h>

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

void det_focus_stop(const char* why) {
    if (!focus_active()) return;
    focus_stop(why);
    radio_wifi_set_data_filter(false);
    radio_ble_set_focus(false);
}

void det_radio_enable(bool on) {
    if (!on) det_focus_stop("radio off");
    radio_wifi_set_enabled(on);
    radio_ble_set_enabled(on);
}

static bool parse_mac(const char* s, uint8_t out[6]) {
    unsigned v[6];
    char tail;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static void focus_reply(ss_cbor_w_t* w) {
    ss_cbor_map(w, 3);
    ss_cbor_kv_bool(w, "focus", true);
    ss_cbor_kv_text(w, "radio", focus_is_wifi() ? "wifi" : "ble");
    ss_cbor_kv_uint(w, "ch", focus_channel());
}

int det_focus_request(const uint8_t* a, size_t n, ss_cbor_w_t* w) {
    bool on = true;
    (void)ss_cbor_get_bool(a, n, "on", &on);
    if (!on) {
        det_focus_stop("released");
        ss_cbor_map(w, 1);
        ss_cbor_kv_bool(w, "focus", false);
        return SS_OK;
    }

    ss_cbor_val_t m;
    char mac_text[24];
    uint8_t mac[6];
    if (!ss_cbor_map_get(a, n, "mac", &m) || m.type != SS_CBOR_TEXT || m.n == 0 || m.n >= sizeof(mac_text)) return SS_ERR_BADFRAME;
    memcpy(mac_text, m.ptr, m.n);
    mac_text[m.n] = 0;
    if (!parse_mac(mac_text, mac)) return SS_ERR_BADFRAME;

    // A repeat request for the target we already hold is just a lease refresh.
    if (focus_matches(mac)) {
        focus_refresh();
        focus_reply(w);
        return SS_OK;
    }

    // What does the registry know about it? If nothing (a pivot to a peer), the request must say.
    bool known = false, wifi = false;
    uint32_t ch = 0;
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return SS_ERR_BUSY;
    for (int i = 0; i < g_reg.count; i++) {
        const Detection& e = g_reg.items[i];
        if (memcmp(e.mac, mac, 6) != 0) continue;
        known = true;
        wifi = (e.proto & PROTO_WIFI) != 0;
        ch = (uint32_t)e.channel;
        break;
    }
    xSemaphoreGive(g_reg_mutex);

    ss_cbor_val_t rv;
    if (ss_cbor_map_get(a, n, "radio", &rv) && rv.type == SS_CBOR_TEXT) {       // an explicit radio wins
        if (rv.n == 4 && memcmp(rv.ptr, "wifi", 4) == 0) wifi = true;
        else if (rv.n == 3 && memcmp(rv.ptr, "ble", 3) == 0) wifi = false;
        else return SS_ERR_BADFRAME;
    } else if (!known) {
        return SS_ERR_BADFRAME;
    }
    uint32_t reqch = 0;
    if (ss_cbor_get_uint(a, n, "ch", &reqch) && reqch) ch = reqch;
    if (wifi && (ch < 1 || ch > 165)) return SS_ERR_BADFRAME;                  // need a channel to park on

    if (!known) {                                                              // pivot: list it as a low-confidence peer
        Detection d;
        d.type = ThreatType::UNKNOWN;
        snprintf(d.name, sizeof(d.name), "%s", "Peer (pivot)");
        memcpy(d.mac, mac, 6);
        d.proto = wifi ? PROTO_WIFI : PROTO_BLE;
        d.addr_type = wifi ? ((mac[0] & 0x02) ? ADDR_WIFI_LOCAL : ADDR_WIFI_UNIVERSAL) : ADDR_UNKNOWN;
        d.channel = wifi ? (int)ch : 0;
        d.band = wifi ? (ch >= 36 ? 5 : 2) : 0;
        d.timestamp_ms = ms_now();
        if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return SS_ERR_BUSY;
        int idx = g_reg.upsert(d);
        xSemaphoreGive(g_reg_mutex);
        if (idx < 0) ESP_LOGW("focus", "registry full: focusing without a list entry");   // focus does not need one
    }

    det_focus_stop("retarget");
    focus_start(mac, wifi, (uint8_t)ch);
    if (wifi) radio_wifi_set_data_filter(true);
    else radio_ble_set_focus(true);
    focus_reply(w);
    return SS_OK;
}

// RSSI_TICK body for the HUNT view: a few per second while focused.
bool det_pop_focus_event(ss_cbor_w_t* w) {
    static uint32_t last = 0;
    uint32_t now = ms_now();
    if (!focus_active() || now - last < 200) return false;
    static FocusState f;
    if (!focus_snapshot(&f)) return false;
    last = now;
    char mac[20];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
    ss_cbor_map(w, 4);
    ss_cbor_kv_text(w, "mac", mac);
    ss_cbor_kv_int(w, "rssi", f.last_rssi);
    ss_cbor_kv_uint(w, "age_ms", f.last_tx_ms ? now - f.last_tx_ms : 99999);
    ss_cbor_kv_uint(w, "n", f.wifi ? f.frames : f.adv_count);
    return !w->err;
}

// GET_DEVICE_PROFILE view "focus": what the parked radio has collected about the target.
bool det_encode_focus(ss_cbor_w_t* w) {
    static FocusState f;                             // link task only; keep off the stack
    if (!focus_snapshot(&f)) return false;
    uint32_t now = ms_now();
    char mac[20];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);

    size_t pairs = 7;                                // mac radio ch up_ms age_ms rssi n
    if (f.wifi) pairs += 7;                          // beacons presp preq omgmt data iemap peers (ht, vie below)
    else pairs += 2;                                 // advs, adv_int
    if (f.wifi && f.ht_streams) pairs++;
    if (f.wifi && f.vie_count) pairs++;
    if (!f.wifi && f.raw_len) pairs++;
    ss_cbor_map(w, pairs);
    ss_cbor_kv_text(w, "mac", mac);
    ss_cbor_kv_text(w, "radio", f.wifi ? "wifi" : "ble");
    ss_cbor_kv_uint(w, "ch", f.channel);
    ss_cbor_kv_uint(w, "up_ms", now - f.started_ms);
    ss_cbor_kv_uint(w, "age_ms", f.last_tx_ms ? now - f.last_tx_ms : 99999);
    ss_cbor_kv_int(w, "rssi", f.last_rssi);
    ss_cbor_kv_uint(w, "n", f.wifi ? f.frames : f.adv_count);
    if (f.wifi) {
        ss_cbor_kv_uint(w, "beacons", f.beacons);
        ss_cbor_kv_uint(w, "presp", f.probe_resps);
        ss_cbor_kv_uint(w, "preq", f.probe_reqs);
        ss_cbor_kv_uint(w, "omgmt", f.other_mgmt);
        ss_cbor_kv_uint(w, "data", f.data_frames);
        ss_cbor_kv_bytes(w, "iemap", f.ie_map, sizeof(f.ie_map));
        if (f.ht_streams) ss_cbor_kv_uint(w, "ht", f.ht_streams);
        if (f.vie_count) ss_cbor_kv_bytes(w, "vie", &f.vie[0][0], (size_t)f.vie_count * 4);
        // peers, busiest first
        uint8_t order[FOCUS_MAX_PEERS];
        int np = f.peer_count;
        for (int i = 0; i < np; i++) order[i] = (uint8_t)i;
        for (int i = 1; i < np; i++) {
            uint8_t k = order[i];
            int j = i - 1;
            auto tot = [&](uint8_t x) { return f.peers[x].to_frames + f.peers[x].from_frames; };
            while (j >= 0 && tot(order[j]) < tot(k)) { order[j + 1] = order[j]; j--; }
            order[j + 1] = k;
        }
        ss_cbor_text(w, "peers");
        ss_cbor_array(w, (size_t)np);
        for (int i = 0; i < np; i++) {
            const FocusPeer& p = f.peers[order[i]];
            char pm[20];
            snprintf(pm, sizeof(pm), "%02X:%02X:%02X:%02X:%02X:%02X", p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
            ss_cbor_map(w, 9);
            ss_cbor_kv_text(w, "mac", pm);
            ss_cbor_kv_uint(w, "role", p.role);
            ss_cbor_kv_int(w, "rssi", p.rssi);
            ss_cbor_kv_uint(w, "tof", p.to_frames);
            ss_cbor_kv_uint(w, "frf", p.from_frames);
            ss_cbor_kv_uint(w, "tob", p.to_bytes);
            ss_cbor_kv_uint(w, "frb", p.from_bytes);
            ss_cbor_kv_uint(w, "qos", p.qos_frames);
            ss_cbor_kv_uint(w, "age_ms", now - p.last_ms);
        }
    } else {
        ss_cbor_kv_uint(w, "advs", f.adv_count);
        ss_cbor_kv_uint(w, "adv_int", f.adv_int_min);
        if (f.raw_len) ss_cbor_kv_bytes(w, "raw", f.raw, f.raw_len);
    }
    return !w->err;
}

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
    static uint32_t last_prune = 0, last_log = 0, last_sync = 0;
    uint32_t now = ms_now();
    if (focus_active() && focus_expired(now)) det_focus_stop("lease lapsed");   // the CYD stopped refreshing it
    if (focus_active() && now - last_sync >= 500) {                            // keep the focused entry live (pivoted peers never re-match a rule)
        last_sync = now;
        static FocusState f;
        if (focus_snapshot(&f) && f.last_tx_ms && xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (int i = 0; i < g_reg.count; i++)
                if (memcmp(g_reg.items[i].mac, f.mac, 6) == 0) {
                    g_reg.items[i].rssi = f.last_rssi;
                    g_reg.items[i].timestamp_ms = f.last_tx_ms;
                    break;
                }
            xSemaphoreGive(g_reg_mutex);
        }
    }
    if (now - last_prune >= 1000) { last_prune = now; registry_prune(); }
    // Stats line: every 5 s while scanning, every 60 s in charge mode (fewer wake-ups)
    uint32_t log_every = radio_wifi_enabled() ? 5000 : 60000;
    if (now - last_log >= log_every) { last_log = now; radio_wifi_log_stats(); radio_ble_log_stats(); }
}

}  // extern "C"
