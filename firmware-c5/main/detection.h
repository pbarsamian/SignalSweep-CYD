// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SS_CONSTRAIN(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

enum class ThreatType { DRONE=0, TRACKER=1, CAMERA=2, BODYCAM=3, ATTACK=4, UNKNOWN=5 };

// Address type, derived from the transmitter address
enum AddrType : uint8_t {
    ADDR_UNKNOWN = 0,
    ADDR_BLE_PUBLIC,        // IEEE-assigned, vendor lookup valid
    ADDR_BLE_RANDOM_STATIC, // fixed until power cycle / reset
    ADDR_BLE_RPA,           // resolvable private, rotates (~15 min)
    ADDR_BLE_NRPA,          // non-resolvable private
    ADDR_WIFI_UNIVERSAL,    // globally unique, vendor lookup valid
    ADDR_WIFI_LOCAL         // locally administered (often randomized)
};

enum WifiFrame : uint8_t { WF_NONE=0, WF_BEACON, WF_PROBE_RESP, WF_PROBE_REQ, WF_NAN };
enum WifiSec : uint8_t { SEC_UNKNOWN=0, SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA2_WPA3,
                         SEC_WPA3, SEC_WPA2_ENT, SEC_WPA3_ENT, SEC_OWE };
// 802.11 generation bits
#define WSTD_N  0x01
#define WSTD_AC 0x02
#define WSTD_AX 0x04
#define WSTD_BE 0x08

#define PROTO_BLE  0x01
#define PROTO_WIFI 0x02

#define TXP_NONE 127

struct RemoteID {
    char    uas_id[21]      = {};
    char    operator_id[21] = {};
    uint8_t ua_type         = 0;
    bool    loc_valid       = false;
    bool    op_valid        = false;
    float   lat = 0, lon = 0;
    float   height_agl  = -1000;  // ODID invalid = -1000
    float   alt_geo     = -1000;
    float   speed_ms    = 255;    // ODID invalid = 255
    float   vspeed_ms   = 63;     // ODID invalid = 63
    float   heading     = 361;    // ODID invalid = 361
    float   op_lat = 0, op_lon = 0;
};

#define MAX_SVC 4

struct Detection {
    // identity / match
    ThreatType type          = ThreatType::UNKNOWN;
    char       name[32]      = {};   // matched rule name
    uint8_t    mac[6]        = {};
    uint8_t    addr_type     = ADDR_UNKNOWN;
    uint8_t    proto         = 0;    // PROTO_BLE | PROTO_WIFI
    uint8_t    confidence    = 0;    // upstream: sum of matched signals (list)
    uint8_t    best_weight   = 0;    // upstream: strongest single signal (alert)
    bool       muted         = false;
    bool       alerted       = false;
    bool       corroborated  = false;
    // signal / timing
    int        rssi          = -100;
    int8_t     rssi_min      = 0;
    int8_t     rssi_max      = -128;
    uint32_t   first_seen_ms = 0;
    uint32_t   timestamp_ms  = 0;    // last seen
    uint32_t   seen_count    = 0;
    int        channel       = 0;    // WiFi channel (1-14 or 5 GHz 36-165), 0 = BLE
    uint8_t    band          = 0;    // 2 = 2.4 GHz, 5 = 5 GHz, 0 = BLE
    // BLE
    char       dev_name[24]  = {};
    int32_t    company       = -1;   // BT SIG company ID
    int8_t     tx_power      = TXP_NONE;
    uint16_t   appearance    = 0;
    bool       has_appearance= false;
    bool       connectable   = false;
    uint8_t    apple_type    = 0xFF; // Continuity type, 0xFF = none
    uint8_t    apple_info    = 0;
    uint16_t   svc[MAX_SVC]  = {};   // 16-bit service / service-data UUIDs
    uint8_t    svc_count     = 0;
    bool       svc_128       = false;// also advertises a 128-bit UUID
    // WiFi
    char       ssid[33]      = {};   // own network (beacon/probe resp)
    char       probe_ssid[33]= {};   // network a client is probing for
    bool       hidden_ssid   = false;
    uint8_t    wifi_frame    = WF_NONE;
    uint8_t    wifi_sec      = SEC_UNKNOWN;
    uint8_t    wifi_std      = 0;
    uint16_t   beacon_int    = 0;    // TU
    char       country[3]    = {};
    // drone
    bool       has_remote_id = false;
    RemoteID   remote_id     = {};

    void mac_str(char* buf) const {
        snprintf(buf, 20, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    int rssi_bars() const {
        if (rssi >= -60) return 5;
        if (rssi >= -70) return 4;
        if (rssi >= -80) return 3;
        if (rssi >= -90) return 2;
        return 1;
    }
};

#define MAX_DETECTIONS 32
#define DETECTION_STALE_MS 120000   // upstream WATCHERS_STALE_MS
#define W_CORROBORATION 15          // upstream: same MAC on BLE and WiFi

struct DetectionRegistry {
    Detection items[MAX_DETECTIONS];
    int       count = 0;

    static void merge_str(char* dst, const char* src, size_t n) {
        if (src[0]) { strncpy(dst, src, n - 1); dst[n - 1] = 0; }
    }

    int upsert(const Detection& d) {
        int8_t r8 = (int8_t)SS_CONSTRAIN(d.rssi, -127, 0);
        for (int i = 0; i < count; i++) {
            if (memcmp(items[i].mac, d.mac, 6) != 0) continue;
            Detection& e = items[i];
            e.rssi = d.rssi;
            if (r8 < e.rssi_min) e.rssi_min = r8;
            if (r8 > e.rssi_max) e.rssi_max = r8;
            e.timestamp_ms = d.timestamp_ms;
            e.seen_count++;
            if (d.channel) { e.channel = d.channel; e.band = d.band; }
            if (d.addr_type) e.addr_type = d.addr_type;
            e.proto |= d.proto;
            if (d.has_remote_id) { e.has_remote_id = true; e.remote_id = d.remote_id; }
            if (d.confidence > e.confidence) e.confidence = d.confidence;
            // Upstream corroboration: same MAC heard on BLE and WiFi
            if (!e.corroborated && e.proto == (PROTO_BLE | PROTO_WIFI)) {
                e.corroborated = true;
                e.confidence = (uint8_t)(e.confidence + W_CORROBORATION > 100 ? 100 : e.confidence + W_CORROBORATION);
            }
            // A later, weaker match must not overwrite a stronger identification
            if (d.best_weight >= e.best_weight) {
                e.best_weight = d.best_weight;
                e.type = d.type;
                memcpy(e.name, d.name, sizeof(e.name));
            }
            // Keep the best-known value of every descriptive field
            merge_str(e.dev_name, d.dev_name, sizeof(e.dev_name));
            if (d.company >= 0) e.company = d.company;
            if (d.tx_power != TXP_NONE) e.tx_power = d.tx_power;
            if (d.has_appearance) { e.has_appearance = true; e.appearance = d.appearance; }
            if (d.connectable) e.connectable = true;
            if (d.apple_type != 0xFF) { e.apple_type = d.apple_type; e.apple_info = d.apple_info; }
            for (int s = 0; s < d.svc_count; s++) {
                bool have = false;
                for (int k = 0; k < e.svc_count; k++) if (e.svc[k] == d.svc[s]) have = true;
                if (!have && e.svc_count < MAX_SVC) e.svc[e.svc_count++] = d.svc[s];
            }
            if (d.svc_128) e.svc_128 = true;
            merge_str(e.ssid, d.ssid, sizeof(e.ssid));
            merge_str(e.probe_ssid, d.probe_ssid, sizeof(e.probe_ssid));
            if (d.hidden_ssid) e.hidden_ssid = true;
            if (d.wifi_frame) e.wifi_frame = d.wifi_frame;
            if (d.wifi_sec) e.wifi_sec = d.wifi_sec;
            e.wifi_std |= d.wifi_std;
            if (d.beacon_int) e.beacon_int = d.beacon_int;
            merge_str(e.country, d.country, sizeof(e.country));
            return i;
        }
        if (count < MAX_DETECTIONS) {
            Detection& n = items[count];
            n = d;
            n.muted = false;
            n.alerted = false;
            n.corroborated = false;
            n.first_seen_ms = d.timestamp_ms;
            n.seen_count = 1;
            n.rssi_min = n.rssi_max = r8;
            return count++;
        }
        return -1;
    }

    void prune(uint32_t now_ms, uint32_t max_age_ms = DETECTION_STALE_MS) {
        for (int i = 0; i < count; ) {
            if (now_ms - items[i].timestamp_ms > max_age_ms) {
                for (int j = i; j < count-1; j++) {
                    items[j] = items[j+1];
                }
                count--;
            } else i++;
        }
    }
};
