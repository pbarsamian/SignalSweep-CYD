// SPDX-License-Identifier: GPL-3.0-or-later
// radio_wifi.cpp — passive dual-band Wi-Fi capture + detection (C5 port).
// Detection logic is the CYD standalone firmware's promiscuous callback, itself
// ported from upstream B0z0dcl0wn/SignalSweep watchersWifiPromiscuousCallback
// (GPL-3.0-or-later): OUI rules, Flock wildcard-probe signature, "Flock-xxxxxx"
// hotspot SSID, SSID-prefix rules, plus the CYD additions (DJI DroneID IE 26:37:12,
// Tello/ANAFI SSIDs). NOT yet ported: Remote ID decode (beacon IE is flagged but not
// decoded; NAN action frames are skipped) — arrives with GET_DEVICE_PROFILE.
#include "radio_wifi.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "registry.h"
#include "signatures.h"

static const char* TAG = "wifi";

// ---- channel plan: 2.4 GHz then 5 GHz, one dwell each ------------------------
static const uint8_t PLAN[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
static const size_t NPLAN = sizeof(PLAN);
#define DWELL_MS 110                         // just over one beacon interval (102.4 ms)

static volatile uint8_t  s_cur_ch = 0;
static volatile uint32_t s_sweeps = 0;
static uint64_t s_bad_ch_mask = 0;           // channels the driver refused (logged once)
static volatile bool s_enabled = false;      // radio on? (off in charge mode)
static SemaphoreHandle_t s_radio_mux;        // serialises enable/disable against the hop task

// ---- AP table ------------------------------------------------------------------
#define MAX_APS 32
struct ApRec {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  ch, band, sec;
    int8_t   rssi;
    uint32_t last_ms;
    bool     used;
};
static ApRec s_aps[MAX_APS];
static portMUX_TYPE s_ap_mux = portMUX_INITIALIZER_UNLOCKED;

static void ap_note(const uint8_t* bssid, const char* ssid, int ch, int rssi, uint8_t sec) {
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_ap_mux);
    int slot = -1, freeslot = -1, oldest = 0;
    for (int i = 0; i < MAX_APS; i++) {
        if (s_aps[i].used && memcmp(s_aps[i].bssid, bssid, 6) == 0) { slot = i; break; }
        if (!s_aps[i].used && freeslot < 0) freeslot = i;
        if (s_aps[i].last_ms < s_aps[oldest].last_ms) oldest = i;
    }
    if (slot < 0) slot = freeslot >= 0 ? freeslot : oldest;   // new AP: free slot, else evict the stalest
    ApRec& a = s_aps[slot];
    if (!a.used || memcmp(a.bssid, bssid, 6) != 0) {            // new entry (or evicted slot): start clean
        memset(&a, 0, sizeof(a)); memcpy(a.bssid, bssid, 6); a.used = true;
    }
    if (ssid[0]) { strncpy(a.ssid, ssid, sizeof(a.ssid) - 1); a.ssid[sizeof(a.ssid) - 1] = 0; }
    a.ch = (uint8_t)ch;
    a.band = ch >= 36 ? 5 : 2;
    a.rssi = (int8_t)SS_CONSTRAIN(rssi, -127, 0);
    a.sec = sec;
    a.last_ms = now;
    portEXIT_CRITICAL(&s_ap_mux);
}

uint32_t radio_wifi_ap_count(void) {
    uint32_t now = ms_now(), n = 0;
    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < MAX_APS; i++) if (s_aps[i].used && now - s_aps[i].last_ms < 60000) n++;
    portEXIT_CRITICAL(&s_ap_mux);
    return n;
}

bool radio_wifi_encode_aps(ss_cbor_w_t* w) {
    static ApRec snap[MAX_APS];                  // link task only; keep off the stack
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_ap_mux);
    memcpy(snap, s_aps, sizeof(snap));
    portEXIT_CRITICAL(&s_ap_mux);

    int order[MAX_APS], n = 0;
    for (int i = 0; i < MAX_APS; i++) if (snap[i].used && now - snap[i].last_ms < 60000) order[n++] = i;
    for (int i = 1; i < n; i++) {                // freshest first (insertion sort)
        int k = order[i], j = i - 1;
        while (j >= 0 && snap[order[j]].last_ms < snap[k].last_ms) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    if (n > 20) n = 20;                          // keeps the reply well under one frame

    ss_cbor_map(w, 3);
    ss_cbor_kv_bool(w, "canned", false);
    ss_cbor_kv_uint(w, "total", n);
    ss_cbor_text(w, "aps");
    ss_cbor_array(w, n);
    for (int i = 0; i < n; i++) {
        const ApRec& a = snap[order[i]];
        char mac[20];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5]);
        ss_cbor_map(w, 7);
        ss_cbor_kv_text(w, "mac", mac);
        ss_cbor_kv_text(w, "ssid", a.ssid);
        ss_cbor_kv_uint(w, "ch", a.ch);
        ss_cbor_kv_uint(w, "band", a.band);
        ss_cbor_kv_int(w, "rssi", a.rssi);
        ss_cbor_kv_uint(w, "sec", a.sec);
        ss_cbor_kv_uint(w, "age_ms", now - a.last_ms);
    }
    return !w->err;
}

// ---- detection helpers (ported) ------------------------------------------------
struct WifiInfo {
    uint8_t frame = WF_NONE;
    uint8_t sec = SEC_UNKNOWN;
    uint8_t std = 0;
    uint16_t bint = 0;
    bool hidden = false;
    char ssid[33] = {};
    char probe[33] = {};
    char country[3] = {};
};

static void post(const uint8_t mac[6], int rssi, int ch, const char* rule, const char* category,
                 int confidence, int best, const WifiInfo* wi) {
    STAT_INC(g_stats.wifi_hit);
    ThreatType t = threat_from_category(category);
    if (t == ThreatType::UNKNOWN || !type_enabled(t, g_cfg)) { STAT_INC(g_stats.wifi_drop); return; }
    Detection d{};
    d.type = t;
    d.rssi = rssi;
    d.channel = ch;
    d.band = ch >= 36 ? 5 : 2;
    d.timestamp_ms = ms_now();
    d.proto = PROTO_WIFI;
    memcpy(d.mac, mac, 6);
    // Locally-administered bit set = randomized / not an IEEE vendor address
    d.addr_type = (mac[0] & 0x02) ? ADDR_WIFI_LOCAL : ADDR_WIFI_UNIVERSAL;
    snprintf(d.name, sizeof(d.name), "%s", rule);
    if (wi) {
        d.wifi_frame = wi->frame;
        d.wifi_sec = wi->sec;
        d.wifi_std = wi->std;
        d.beacon_int = wi->bint;
        d.hidden_ssid = wi->hidden;
        memcpy(d.ssid, wi->ssid, sizeof(d.ssid));
        memcpy(d.probe_ssid, wi->probe, sizeof(d.probe_ssid));
        memcpy(d.country, wi->country, sizeof(d.country));
    }
    post_detection(d, confidence, best);
}

// RSN element -> security class (AKM suites 00-0F-AC:xx)
static uint8_t parse_rsn(const uint8_t* e, int len) {
    // version(2) group(4) pairwise_count(2) pairwise(4n) akm_count(2) akm(4m)
    if (len < 8) return SEC_WPA2;
    int p = 6;
    int pc = e[p] | (e[p+1] << 8); p += 2 + 4 * pc;
    if (p + 2 > len) return SEC_WPA2;
    int ac = e[p] | (e[p+1] << 8); p += 2;
    bool psk = false, sae = false, eap = false, eap3 = false, owe = false;
    for (int i = 0; i < ac && p + 4 <= len; i++, p += 4) {
        if (!(e[p]==0x00 && e[p+1]==0x0F && e[p+2]==0xAC)) continue;
        switch (e[p+3]) {
            case 1: case 3: case 5: eap = true; break;   // 802.1X / FT / SHA256
            case 2: case 4: case 6: psk = true; break;   // PSK / FT-PSK / SHA256
            case 8: case 9: case 24: case 25: sae = true; break; // SAE / FT-SAE / SAE-EXT
            case 12: case 13: eap3 = true; break;        // Suite-B 192 (WPA3-Ent)
            case 18: owe = true; break;                  // OWE (Enhanced Open)
        }
    }
    if (eap3) return SEC_WPA3_ENT;
    if (sae && psk) return SEC_WPA2_WPA3;
    if (sae) return SEC_WPA3;
    if (owe) return SEC_OWE;
    if (eap) return SEC_WPA2_ENT;
    return SEC_WPA2;
}

// ---- the promiscuous callback (runs in the Wi-Fi task: keep it light) ----------
static void promisc(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    STAT_INC(g_stats.wifi_mgmt);
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t* payload = pkt->payload;
    int length = pkt->rx_ctrl.sig_len;
    int rssi   = pkt->rx_ctrl.rssi;
    int rxCh   = pkt->rx_ctrl.channel;
    if (length < 24) return;

    uint8_t fsubtype = (payload[0] >> 4) & 0x0F;
    uint8_t* addr2 = payload + 10;

    // Probe request (4), probe response (5), beacon (8)
    if (!(fsubtype == 4 || fsubtype == 5 || fsubtype == 8)) return;

    int conf = 0, best = 0;
    const char* rule = nullptr;
    const char* cat  = nullptr;
    bool flockOui = false;
    int dsCh = 0;
    char foundSsid[33] = {0};
    WifiInfo wi;
    wi.frame = fsubtype == 8 ? WF_BEACON : fsubtype == 5 ? WF_PROBE_RESP : WF_PROBE_REQ;
    bool privacy = false, rsn = false, wpa1 = false;
    uint8_t rsnSec = SEC_WPA2;
    if (fsubtype != 4 && length >= 36) {
        // Fixed params: timestamp(8) beacon interval(2) capability(2)
        wi.bint = payload[32] | (payload[33] << 8);
        privacy = payload[34] & 0x10;
    }

    // 1. OUI rules — first match only; a rule's own weight applies only when
    //    the OUI is its whole condition (other fields are BLE-only)
    for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
        const Signature& s = SIGNATURES[i];
        if (!s.oui[0] || s.ssid[0] || sig_gated_off(s, g_cfg)) continue;
        if (!oui_matches(addr2, s.oui)) continue;
        bool ouiOnly = !s.mfg[0] && !s.dev[0] && !s.uuid[0];
        int w = (ouiOnly && s.weight > 0) ? s.weight : W_WIFI_OUI;
        conf += w;
        if (strcmp(s.category, "Flock Safety") == 0) flockOui = true;
        if (w > best) { best = w; rule = s.name; cat = s.category; }
        break;
    }

    // 2. Information-element walk
    if (length > 24 + 12) {
        int offset = (fsubtype == 4) ? 24 : 36;
        int body_len = length - offset - 4;   // -4 FCS
        const uint8_t* body = payload + offset;
        bool ieHit = false, ssidHit = false, wildcard = false, liteonSig = false;
        int b = 0;
        while (b < body_len - 1) {
            uint8_t id = body[b], elen = body[b+1];
            if (b + 2 + elen > body_len) break;
            const uint8_t* e = body + b + 2;

            if (id == 3 && elen == 1) dsCh = e[0];
            if (id == 48) { rsn = true; rsnSec = parse_rsn(e, elen); }
            if (id == 221 && elen >= 4 && e[0]==0x00 && e[1]==0x50 && e[2]==0xF2 && e[3]==0x01) wpa1 = true;
            if (id == 45)  wi.std |= WSTD_N;                    // HT capabilities
            if (id == 191) wi.std |= WSTD_AC;                   // VHT capabilities
            if (id == 255 && elen >= 1 && e[0] == 35)  wi.std |= WSTD_AX; // HE capabilities
            if (id == 255 && elen >= 1 && e[0] == 108) wi.std |= WSTD_BE; // EHT capabilities
            if (id == 7 && elen >= 2 && isalpha(e[0]) && isalpha(e[1])) {
                wi.country[0] = (char)e[0]; wi.country[1] = (char)e[1]; wi.country[2] = 0;
            }

            // Flock stack's exact Lite-On vendor IE (tightener only)
            if (id == 221 && elen == 7 && e[0]==0x50 && e[1]==0x6F && e[2]==0x9A &&
                e[3]==0x16 && e[4]==0x03 && e[5]==0x01 && e[6]==0x03) liteonSig = true;

            // Weak Lite-On IE hint: adds confidence, never a category
            if (!ieHit && id == 221 && elen >= 4 && e[0]==0x50 && e[1]==0x6F && e[2]==0x9A) {
                ieHit = true;
                conf += W_WIFI_IE;
                if (W_WIFI_IE > best) { best = W_WIFI_IE; rule = "Lite-On Vendor IE (weak)"; }
            }

            // Remote ID in a beacon vendor IE (ASTM 90:3A:E6 / French FA:0B:BC).
            // Flagged here; the payload decode comes with GET_DEVICE_PROFILE.
            if (id == 221 && elen >= 6 &&
                ((e[0]==0x90 && e[1]==0x3A && e[2]==0xE6) ||
                 (e[0]==0xFA && e[1]==0x0B && e[2]==0xBC))) {
                conf += W_DRONE;
                if (W_DRONE > best) { best = W_DRONE; rule = "Remote ID Drone"; cat = "Drone"; }
            }

            // DJI DroneID (proprietary, pre-Remote-ID): vendor IE OUI 26:37:12,
            // subcommand 0x10 telemetry / 0x11 flight info (Kismet's
            // dot11_ie_221_dji_droneid layout: OUI, vendor type, 2 bytes, subcmd)
            if (id == 221 && elen >= 7 && e[0]==0x26 && e[1]==0x37 && e[2]==0x12 &&
                (e[6] == 0x10 || e[6] == 0x11)) {
                conf += W_DRONE;
                if (W_DRONE > best) { best = W_DRONE; rule = "DJI DroneID"; cat = "Drone"; }
            }

            // Zero-length SSID on a probe request = wildcard probe
            if (id == 0 && elen == 0 && fsubtype == 4) wildcard = true;

            if (id == 0 && (fsubtype == 8 || fsubtype == 5)) {
                // Hidden network: empty or all-zero SSID in a beacon
                bool allz = true;
                for (int k = 0; k < elen; k++) if (e[k]) allz = false;
                if (elen == 0 || allz) wi.hidden = true;
            }
            if (id == 0 && elen > 0 && elen <= 32) {
                char ssid[33] = {0};
                memcpy(ssid, e, elen);
                // Only beacons/probe responses name the sender's own network
                if (fsubtype == 8 || fsubtype == 5) memcpy(foundSsid, ssid, sizeof(ssid));
                else if (ssid[0]) memcpy(wi.probe, ssid, sizeof(ssid));   // probe request target
                // Falcon hotspot: "Flock-" + last 6 of MAC, exactly 12 chars
                if (!ssidHit && elen == 12 && strncasecmp(ssid, "flock-", 6) == 0) {
                    ssidHit = true;
                    conf += W_WIFI_SSID;
                    if (W_WIFI_SSID > best) { best = W_WIFI_SSID; rule = "Flock SSID Signature"; cat = "Flock Safety"; }
                }
            }
            b += 2 + elen;
        }

        // Flock wildcard probe: only meaningful from a Flock-listed OUI
        if (flockOui && wildcard) {
            int w = liteonSig ? W_WIFI_IE_SIG : W_WIFI_PROBE;
            conf += w;
            if (w > best) { best = w; rule = liteonSig ? "Flock probe + IE" : "Flock wildcard probe"; cat = "Flock Safety"; }
        }
    }

    // Track every beacon / probe response in the AP table (feeds SCAN_WIFI), whether
    // or not any rule matched.
    if (fsubtype != 4) {
        uint8_t sec = rsn ? rsnSec : wpa1 ? (uint8_t)SEC_WPA : privacy ? (uint8_t)SEC_WEP : (uint8_t)SEC_OPEN;
        ap_note(addr2, foundSsid, dsCh ? dsCh : rxCh, rssi, sec);
    }

    // 3. SSID-prefix rules (beacons / probe responses only), first match wins
    if (foundSsid[0]) {
        for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
            const Signature& s = SIGNATURES[i];
            if (!s.ssid[0] || sig_gated_off(s, g_cfg)) continue;
            if (strncmp(foundSsid, s.ssid, strlen(s.ssid)) != 0) continue;
            if (s.oui[0] && !oui_matches(addr2, s.oui)) continue;
            bool ouiOnly = !s.mfg[0] && !s.dev[0] && !s.uuid[0];
            int w = (ouiOnly && s.weight > 0) ? s.weight : W_WIFI_SSID;
            conf += w;
            if (w > best) { best = w; rule = s.name; cat = s.category; }
            break;
        }
    }

    if (!rule || !cat) return;   // weak IE alone never names a category
    if (fsubtype != 4) {
        wi.sec = rsn ? rsnSec : wpa1 ? (uint8_t)SEC_WPA : privacy ? (uint8_t)SEC_WEP : (uint8_t)SEC_OPEN;
        memcpy(wi.ssid, foundSsid, sizeof(wi.ssid));
    }
    post(addr2, rssi, dsCh ? dsCh : rxCh, rule, cat, conf, best, &wi);
}

// ---- channel hopping --------------------------------------------------------------
static void hop_task(void*) {
    size_t i = 0;
    for (;;) {
        if (!s_enabled) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }   // charge mode: idle
        uint8_t ch = PLAN[i];
        xSemaphoreTake(s_radio_mux, portMAX_DELAY);
        esp_err_t e = s_enabled ? esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_radio_mux);
        if (!s_enabled) continue;
        if (e == ESP_OK) {
            s_cur_ch = ch;
            vTaskDelay(pdMS_TO_TICKS(DWELL_MS));
        } else {
            if (!(s_bad_ch_mask & (1ULL << (i & 63)))) {
                s_bad_ch_mask |= 1ULL << (i & 63);
                ESP_LOGW(TAG, "channel %u refused by the driver (%s); skipping", ch, esp_err_to_name(e));
            }
            vTaskDelay(1);
        }
        if (++i >= NPLAN) { i = 0; s_sweeps = s_sweeps + 1; }
    }
}

void radio_wifi_log_stats(void) {
    static uint32_t last_mgmt = 0;
    uint32_t m = g_stats.wifi_mgmt;
    uint32_t n2 = 0, n5 = 0, now = ms_now();
    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < MAX_APS; i++)
        if (s_aps[i].used && now - s_aps[i].last_ms < 60000) { if (s_aps[i].band == 5) n5++; else n2++; }
    portEXIT_CRITICAL(&s_ap_mux);
    ESP_LOGI(TAG, "radio=%s sweeps=%u ch=%u mgmt=%u (+%u) APs 2.4G=%u 5G=%u hits=%u dets=%d",
             s_enabled ? "ON" : "OFF", (unsigned)s_sweeps, (unsigned)s_cur_ch, (unsigned)m, (unsigned)(m - last_mgmt),
             (unsigned)n2, (unsigned)n5, (unsigned)g_stats.wifi_hit, g_reg.count);
    last_mgmt = m;
}

// Switch the radio fully on or off. Off = promiscuous mode disabled and the Wi-Fi driver
// stopped, which powers the RF front end down. On re-applies the whole capture config.
static void radio_apply(bool on) {
    xSemaphoreTake(s_radio_mux, portMAX_DELAY);
    if (on && !s_enabled) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
        esp_err_t e = esp_wifi_set_country_code("US", false);   // receive-only: only picks usable 5 GHz channels
        if (e != ESP_OK) ESP_LOGW(TAG, "set_country_code: %s", esp_err_to_name(e));
        e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (e != ESP_OK) ESP_LOGW(TAG, "set_band_mode(AUTO): %s (5 GHz may be unavailable)", esp_err_to_name(e));
        wifi_promiscuous_filter_t f = {};
        f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous_filter(&f));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous_rx_cb(promisc));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous(true));
        s_enabled = true;
        ESP_LOGI(TAG, "radio ON");
    } else if (!on && s_enabled) {
        s_enabled = false;
        esp_wifi_set_promiscuous(false);
        esp_wifi_stop();
        ESP_LOGW(TAG, "radio OFF (charge mode)");
    }
    xSemaphoreGive(s_radio_mux);
}

void radio_wifi_set_enabled(bool on) { radio_apply(on); }

void radio_wifi_start(bool enabled) {
    s_radio_mux = xSemaphoreCreateMutex();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    xTaskCreate(hop_task, "hop", 3072, nullptr, 4, nullptr);   // idles until the radio is enabled
    // The driver is initialised but NOT started unless allowed: a pack already at the low
    // threshold boots with the RF front end never switched on.
    if (enabled) radio_apply(true);
    ESP_LOGI(TAG, "passive capture ready: %u channels, %d ms dwell, radio %s",
             (unsigned)NPLAN, DWELL_MS, enabled ? "ON" : "OFF (charge mode)");
}
