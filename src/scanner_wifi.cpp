// SignalSweep CYD — WiFi scanner
// Scope: v3.2 behaviour, beacon frames only, tapered hump per channel.
// Detection: ported from upstream B0z0dcl0wn/SignalSweep
// watchersWifiPromiscuousCallback (GPL-3.0-or-later):
//   - OUI rules (first match), Flock wildcard-probe signature (+ Lite-On IE)
//   - "Flock-xxxxxx" hotspot SSID, SSID-prefix rules (Axon pairing, attack APs)
//   - Remote ID over Wi-Fi beacon vendor IE and Wi-Fi NAN (opendroneid)
// CYD addition: DJI DroneID vendor IE (26:37:12)

#include <Arduino.h>
#include <esp_wifi.h>
#include "detection.h"
#include "settings.h"
#include "signatures.h"
#include "odid_util.h"
#include "focus.h"
extern "C" {
#include "odid_wifi.h"
}

extern Settings g_cfg;

static uint8_t  s_pow[320] = {};
static int      s_chi = 0;
static const uint8_t CH[] = {1,2,3,4,5,6,7,8,9,10,11};
static const int     NCH  = sizeof(CH);

// Single-threaded promiscuous callback only (upstream rule): file-static.
static ODID_UAS_Data s_wifiUas;

// Descriptive fields gathered while walking a frame (info view only)
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

static void post(const uint8_t mac[6], int rssi, int ch, const char* rule,
                 const char* category, int confidence, int best, bool drone,
                 const WifiInfo* wi) {
    g_stats.wifi_hit++;
    ThreatType t = threat_from_category(category);
    if (t == ThreatType::UNKNOWN || !type_enabled(t, g_cfg)) { g_stats.wifi_drop++; return; }
    Detection d{};
    d.type = t;
    d.rssi = rssi;
    d.channel = ch;
    d.timestamp_ms = millis();
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
    } else {
        d.wifi_frame = WF_NAN;
    }
    if (drone && odid_useful(s_wifiUas)) odid_apply(d, s_wifiUas);
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

// ── Scope feed: unchanged from v3.10 ────────────────────────────────────────
static void scope_feed(int ch, int rssi) {
    if (ch < 1 || ch > 11) return;
    int x = ((ch-1) * 319) / 10;   // same formula as the channel ticks
    int str = (int)(((rssi+100)/60.0f)*80.0f);
    if (str < 0) str = 0; if (str > 80) str = 80;
    for (int dx=-9; dx<=9; dx++) {
        int xi = x + dx;
        if (xi >= 0 && xi < 320) {
            int v = s_pow[xi] + str/(abs(dx)+1);
            s_pow[xi] = (uint8_t)(v > 100 ? 100 : v);
        }
    }
}

static void promisc(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    g_stats.wifi_mgmt++;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t* payload = pkt->payload;
    int length = pkt->rx_ctrl.sig_len;
    int rssi   = pkt->rx_ctrl.rssi;
    int rxCh   = pkt->rx_ctrl.channel;
    if (length < 24) return;

    uint8_t fsubtype = (payload[0] >> 4) & 0x0F;
    uint8_t* addr1 = payload + 4;
    uint8_t* addr2 = payload + 10;

    // Focus: deep-collect every management frame from the target
    if (focus_target(addr2, FOCUS_WIFI)) focus_note_wifi(fsubtype, payload, length);

    if (fsubtype == 8) scope_feed(rxCh, rssi);   // beacons only, as v3.2

    // Remote ID over Wi-Fi NAN: action frame to the ASTM NAN cluster
    static const uint8_t nanDest[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
    if (memcmp(nanDest, addr1, 6) == 0) {
        memset(&s_wifiUas, 0, sizeof(s_wifiUas));
        char nanMac[6] = {0};
        if (g_cfg.detect_drone &&
            odid_wifi_receive_message_pack_nan_action_frame(&s_wifiUas, nanMac, payload, length) == 0 &&
            odid_useful(s_wifiUas)) {
            post((const uint8_t*)nanMac, rssi, rxCh, "Remote ID Drone", "Drone", W_DRONE, W_DRONE, true, nullptr);
        }
        return;
    }

    // Probe request (4), probe response (5), beacon (8)
    if (!(fsubtype == 4 || fsubtype == 5 || fsubtype == 8)) return;

    int conf = 0, best = 0;
    const char* rule = nullptr;
    const char* cat  = nullptr;
    bool flockOui = false, drone = false;
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
                wi.country[0] = e[0]; wi.country[1] = e[1]; wi.country[2] = 0;
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

            // Remote ID in a beacon vendor IE (ASTM 90:3A:E6 / French FA:0B:BC)
            if (id == 221 && elen >= 6 &&
                ((e[0]==0x90 && e[1]==0x3A && e[2]==0xE6) ||
                 (e[0]==0xFA && e[1]==0x0B && e[2]==0xBC))) {
                drone = true;
                conf += W_DRONE;
                if (W_DRONE > best) { best = W_DRONE; rule = "Remote ID Drone"; cat = "Drone"; }
                int odidLen = (int)elen - 5;   // OUI(3)+type(1)+counter(1)
                if (odidLen > 0) {
                    memset(&s_wifiUas, 0, sizeof(s_wifiUas));
                    odid_decode_into(s_wifiUas, e + 5, (size_t)odidLen);
                }
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
        wi.sec = rsn ? rsnSec : wpa1 ? SEC_WPA : privacy ? SEC_WEP : SEC_OPEN;
        memcpy(wi.ssid, foundSsid, sizeof(wi.ssid));
    }
    post(addr2, rssi, dsCh ? dsCh : rxCh, rule, cat, conf, best, drone, &wi);
}

void scanner_wifi_init(const Settings& s) {
    (void)s;   // live settings are read from g_cfg
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc);
    esp_wifi_set_channel(CH[0], WIFI_SECOND_CHAN_NONE);
    Serial.println("[wifi] init OK");
}

void scanner_wifi_tick(uint8_t* out, int lock_ch) {
    static int s_cur = 0;
    int ch;
    if (lock_ch >= 1 && lock_ch <= 13) {
        ch = lock_ch;                     // focus: stay on the target's channel
    } else {
        s_chi = (s_chi + 1) % NCH;
        ch = CH[s_chi];
    }
    if (ch != s_cur) { esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); s_cur = ch; }
    for (int i=0; i<320; i++) s_pow[i] = s_pow[i] > 4 ? s_pow[i] - 4 : 0;
    memcpy(out, s_pow, 320);
    vTaskDelay(pdMS_TO_TICKS(40));
}
