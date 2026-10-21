// SPDX-License-Identifier: GPL-3.0-or-later
// radio_ble.cpp — passive BLE capture + detection (C5 port).
// Detection logic is the CYD standalone firmware's BLE scanner, itself ported from upstream
// B0z0dcl0wn/SignalSweep mode_watchers_watch.cpp (GPL-3.0-or-later): signature rules with
// upstream's AND semantics and exact UUID matching; Apple Find My (long form 0x12/0x19 only),
// Google FMDN, Tile, Samsung SmartTag; the Flock solar battery ("Penguin"). Remote ID over BLE
// (service data 0xFFFA) is flagged by presence; its payload decode comes with GET_DEVICE_PROFILE.
#include "radio_ble.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "registry.h"
#include "signatures.h"
#include "odid_util.h"
#include "focus.h"
#include "capture.h"

static const char* TAG = "ble";

static volatile bool s_enabled = false;     // scanning wanted (off in charge mode)
static volatile bool s_synced = false;      // NimBLE host ready
static uint8_t s_own_addr_type = 0;
static volatile uint8_t s_duty = 50;         // scan window, percent of the 100 ms interval (the scheduler sets it)
static volatile bool s_paused = false;       // the scheduler has the radio on Wi-Fi right now

// ---- parsed advertisement ----------------------------------------------------------------------
#define MAX_SVC16 8
#define MAX_SVC128 3
#define MAX_SD 6
struct Adv {
    uint8_t  mac[6];                         // MSB first, as printed (NimBLE gives LSB first)
    uint8_t  addr_type;                      // BLE_ADDR_PUBLIC / RANDOM
    int8_t   rssi;
    uint8_t  evtype;
    bool     have_mfg = false;  uint16_t company = 0;  const uint8_t* mfg = nullptr;  uint8_t mfg_len = 0;
    bool     have_name = false; char name[32] = {};
    bool     have_tx = false;   int8_t tx = 0;
    bool     have_appearance = false; uint16_t appearance = 0;
    uint8_t  n16 = 0;  uint16_t svc16[MAX_SVC16] = {};
    uint8_t  n128 = 0; uint8_t svc128[MAX_SVC128][16] = {};      // LSB first, as on the air
    struct { uint16_t uuid; const uint8_t* data; uint8_t len; } sd[MAX_SD] = {};
    uint8_t  nsd = 0;
};

static void parse_adv(const uint8_t* d, uint8_t len, Adv& a) {
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t l = d[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t t = d[i + 1];
        const uint8_t* v = d + i + 2;
        uint8_t vl = l - 1;
        switch (t) {
        case 0x02: case 0x03:                                  // 16-bit service UUIDs
            for (uint8_t k = 0; k + 1 < vl && a.n16 < MAX_SVC16; k += 2) a.svc16[a.n16++] = v[k] | (v[k + 1] << 8);
            break;
        case 0x06: case 0x07:                                  // 128-bit service UUIDs
            for (uint8_t k = 0; k + 16 <= vl && a.n128 < MAX_SVC128; k += 16) memcpy(a.svc128[a.n128++], v + k, 16);
            break;
        case 0x08: case 0x09: {                                // shortened / complete local name
            size_t n = vl < sizeof(a.name) - 1 ? vl : sizeof(a.name) - 1, j = 0;
            for (size_t k = 0; k < n; k++) if (v[k] >= 0x20 && v[k] < 0x7F) a.name[j++] = (char)v[k];
            a.name[j] = 0;
            a.have_name = j > 0;
            break;
        }
        case 0x0A: if (vl >= 1) { a.have_tx = true; a.tx = (int8_t)v[0]; } break;
        case 0x19: if (vl >= 2) { a.have_appearance = true; a.appearance = v[0] | (v[1] << 8); } break;
        case 0x16:                                             // service data, 16-bit UUID
            if (vl >= 2 && a.nsd < MAX_SD) { a.sd[a.nsd].uuid = v[0] | (v[1] << 8); a.sd[a.nsd].data = v + 2; a.sd[a.nsd].len = vl - 2; a.nsd++; }
            break;
        case 0xFF:                                             // manufacturer specific
            if (vl >= 2 && !a.have_mfg) { a.have_mfg = true; a.mfg = v; a.mfg_len = vl; a.company = v[0] | (v[1] << 8); }
            break;
        }
        i += l + 1;
    }
}

static bool find_sd(const Adv& a, uint16_t uuid, const uint8_t** data, uint8_t* len) {
    for (uint8_t k = 0; k < a.nsd; k++)
        if (a.sd[k].uuid == uuid) { if (data) *data = a.sd[k].data; if (len) *len = a.sd[k].len; return true; }
    return false;
}

// ---- upstream uuidMatches(): exact match, never substring ----------------------------------------
static const uint8_t BASE_UUID_LO[12] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };

static bool uuid128_is_base(const uint8_t* u, uint16_t* v16) {
    if (memcmp(u, BASE_UUID_LO, 12) != 0 || u[14] != 0 || u[15] != 0) return false;
    *v16 = u[12] | (u[13] << 8);
    return true;
}

static void uuid128_str(const uint8_t* u, char* out) {                // lowercase 8-4-4-4-12
    static const char* H = "0123456789abcdef";
    int o = 0;
    for (int k = 15; k >= 0; k--) {
        out[o++] = H[u[k] >> 4]; out[o++] = H[u[k] & 15];
        if (k == 12 || k == 10 || k == 8 || k == 6) out[o++] = '-';
    }
    out[o] = 0;
}

static bool uuid_rule_matches(const Adv& a, const char* rule) {
    char want[40]; size_t n = 0;
    for (const char* p = rule; *p && n < sizeof(want) - 1; p++) if (!isspace((unsigned char)*p)) want[n++] = (char)tolower((unsigned char)*p);
    want[n] = 0;
    const char* w = want;
    if (w[0] == '0' && w[1] == 'x') w += 2;
    if (strlen(w) == 4) {
        uint16_t v = (uint16_t)strtoul(w, nullptr, 16);
        for (uint8_t k = 0; k < a.n16; k++) if (a.svc16[k] == v) return true;
        for (uint8_t k = 0; k < a.n128; k++) { uint16_t b; if (uuid128_is_base(a.svc128[k], &b) && b == v) return true; }
        return false;
    }
    for (uint8_t k = 0; k < a.n128; k++) { char s[40]; uuid128_str(a.svc128[k], s); if (strcmp(s, w) == 0) return true; }
    return false;
}

// ---- upstream matchDeviceAgainstRule() ----------------------------------------------------------------
static bool contains_nocase(const char* hay, const char* needle) {
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0 || nn > hn) return nn == 0;
    for (size_t i = 0; i + nn <= hn; i++) if (strncasecmp(hay + i, needle, nn) == 0) return true;
    return false;
}

static int match_rule(const Adv& a, const Signature& s) {
    if (s.ssid[0]) return 0;                           // SSID rules are Wi-Fi only
    if (sig_gated_off(s, g_cfg)) return 0;             // attack gear while Attack is off
    int w = 0;
    if (s.oui[0]) { if (!oui_matches(a.mac, s.oui)) return 0; w += W_OUI; }
    if (s.mfg[0]) { if (!a.have_mfg || a.company != parse_mfg_id(s.mfg)) return 0; w += W_MFG; }
    if (s.dev[0]) { if (!a.have_name || !contains_nocase(a.name, s.dev)) return 0; w += W_NAME; }
    if (s.uuid[0]) { if ((a.n16 + a.n128) == 0 || !uuid_rule_matches(a, s.uuid)) return 0; w += W_UUID; }
    if (w == 0) return 0;
    if (s.weight > 0) w = s.weight;
    return w > 100 ? 100 : w;
}

// ---- upstream bleIsAirtag(): long form only -----------------------------------------------------------
// Every iPhone/iPad/Mac sends the 2-byte 0x12 status ping; only an item separated from its owner
// sends the 25-byte (0x19) offline-finding payload.
static bool ble_is_airtag(const Adv& a) {
    return a.have_mfg && a.mfg_len >= 4 && a.company == 0x004C && a.mfg[2] == 0x12 && a.mfg[3] == 0x19;
}

// ---- upstream bleTrackerNetwork(): FMDN / Tile / Samsung service data ----------------------------------
static const char* ble_tracker_network(const Adv& a) {
    if (a.nsd == 0) return nullptr;
    const uint8_t* d; uint8_t l;
    if (find_sd(a, 0xFEAA, &d, &l) && l >= 1) {
        if (d[0] == 0x41) return "Google Find My (unwanted)";
        if (d[0] == 0x40) return "Google Find My Tracker";
        return nullptr;                                // ordinary Eddystone beacon
    }
    if (find_sd(a, 0xFEED, &d, &l) && l > 0) return "Tile Tracker";
    if (find_sd(a, 0xFD5A, &d, &l) && l > 0) return "Samsung SmartTag";
    if (find_sd(a, 0xFD59, &d, &l) && l > 0) return "Samsung SmartTag (setup)";
    return nullptr;
}

// ---- upstream blePenguin(): Flock solar battery ----------------------------------------------------------
static bool is_ten_digits(const char* s, size_t from) {
    size_t n = strlen(s);
    if (n != from + 10) return false;
    for (size_t i = from; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static bool ble_penguin(const Adv& a) {
    const char* name = a.have_name ? a.name : "";
    if (is_ten_digits(name, 8) && strncasecmp(name, "penguin-", 8) == 0) return true;
    if (!a.have_mfg || a.mfg_len < 2 || a.mfg[0] != 0xC8 || a.mfg[1] != 0x09) return false;
    if (is_ten_digits(name, 0)) return true;
    for (size_t i = 2; i + 2 < a.mfg_len; i++)
        if (a.mfg[i] == 'T' && a.mfg[i + 1] == 'N' && a.mfg[i + 2] >= '0' && a.mfg[i + 2] <= '9') return true;
    return false;
}

// ---- upstream bleDecodeRemoteId(): UUID 0xFFFA service data ------------------------------------------------
// ~1 KB; file-static to keep it off the NimBLE task stack (upstream does the same)
static ODID_UAS_Data s_bleUas;

// Presence alone means a drone broadcasting Remote ID. The ASTM F3411 BT4 framing is: UUID,
// application code 0x0D, counter, then the 25-byte message (or a message pack).
static bool ble_decode_remote_id(const Adv& a, bool* decoded) {
    const uint8_t* d; uint8_t l;
    if (!find_sd(a, 0xFFFA, &d, &l)) return false;
    memset(&s_bleUas, 0, sizeof(s_bleUas));
    *decoded = false;
    if (l >= 2 + ODID_MESSAGE_SIZE && d[0] == 0x0D) {
        odid_decode_into(s_bleUas, d + 2, (size_t)l - 2);
        *decoded = odid_useful(s_bleUas);
    }
    return true;
}

// ---- descriptive fields for the info view (no effect on scoring) -----------------------------------------
static void add_svc(Detection& d, uint16_t v) {
    for (int k = 0; k < d.svc_count; k++) if (d.svc[k] == v) return;
    if (d.svc_count < MAX_SVC) d.svc[d.svc_count++] = v;
}

static void capture_fields(const Adv& a, Detection& d) {
    if (a.addr_type == BLE_ADDR_PUBLIC) d.addr_type = ADDR_BLE_PUBLIC;
    else {
        uint8_t top = d.mac[0] >> 6;
        d.addr_type = top == 3 ? ADDR_BLE_RANDOM_STATIC : top == 1 ? ADDR_BLE_RPA : top == 0 ? ADDR_BLE_NRPA : ADDR_UNKNOWN;
    }
    if (a.have_name) { strncpy(d.dev_name, a.name, sizeof(d.dev_name) - 1); d.dev_name[sizeof(d.dev_name) - 1] = 0; }   // 32 -> 24 bytes, truncated on purpose
    if (a.have_tx) d.tx_power = a.tx;
    if (a.have_appearance) { d.has_appearance = true; d.appearance = a.appearance; }
    // HCI LE advertising report event types: 0 ADV_IND, 1 ADV_DIRECT_IND are connectable;
    // 2 SCAN_IND, 3 NONCONN_IND, 4 SCAN_RSP are not.
    d.connectable = (a.evtype == 0 || a.evtype == 1);
    if (a.have_mfg) {
        d.company = a.company;
        if (a.company == 0x004C && a.mfg_len >= 3) {
            d.apple_type = a.mfg[2];
            if (d.apple_type == 0x12 && a.mfg_len >= 4) d.apple_info = a.mfg[3];
            else if (d.apple_type == 0x10 && a.mfg_len >= 5) d.apple_info = a.mfg[4];
        }
    }
    for (uint8_t k = 0; k < a.n16; k++) add_svc(d, a.svc16[k]);
    for (uint8_t k = 0; k < a.n128; k++) { uint16_t v; if (uuid128_is_base(a.svc128[k], &v)) add_svc(d, v); else d.svc_128 = true; }
    for (uint8_t k = 0; k < a.nsd; k++) add_svc(d, a.sd[k].uuid);
}

// ---- BLE device table (feeds SCAN_BLE) ------------------------------------------------------------------------
#define MAX_BLE 64                                   // distinct devices tracked (stalest evicted beyond this)
struct BleRec { uint8_t mac[6]; char name[24]; uint16_t company; bool has_company; uint8_t addr_type; int8_t rssi; uint32_t last_ms; bool used; };
static BleRec s_devs[MAX_BLE];
static portMUX_TYPE s_dev_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_adv_count = 0;

static void dev_note(const Adv& a) {
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_dev_mux);
    int slot = -1, freeslot = -1, oldest = 0;
    for (int i = 0; i < MAX_BLE; i++) {
        if (s_devs[i].used && memcmp(s_devs[i].mac, a.mac, 6) == 0) { slot = i; break; }
        if (!s_devs[i].used && freeslot < 0) freeslot = i;
        if (s_devs[i].last_ms < s_devs[oldest].last_ms) oldest = i;
    }
    if (slot < 0) slot = freeslot >= 0 ? freeslot : oldest;
    BleRec& r = s_devs[slot];
    if (!r.used || memcmp(r.mac, a.mac, 6) != 0) { memset(&r, 0, sizeof(r)); memcpy(r.mac, a.mac, 6); r.used = true; }
    if (a.have_name) { strncpy(r.name, a.name, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0; }
    if (a.have_mfg) { r.company = a.company; r.has_company = true; }
    r.addr_type = a.addr_type;
    r.rssi = (int8_t)SS_CONSTRAIN((int)a.rssi, -127, 0);
    r.last_ms = now;
    portEXIT_CRITICAL(&s_dev_mux);
}

bool radio_ble_encode_devs(ss_cbor_w_t* w) {
    static BleRec snap[MAX_BLE];
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_dev_mux);
    memcpy(snap, s_devs, sizeof(snap));
    portEXIT_CRITICAL(&s_dev_mux);
    int order[MAX_BLE], n = 0;
    for (int i = 0; i < MAX_BLE; i++) if (snap[i].used && now - snap[i].last_ms < 60000) order[n++] = i;
    for (int i = 1; i < n; i++) {                                    // freshest first
        int k = order[i], j = i - 1;
        while (j >= 0 && snap[order[j]].last_ms < snap[k].last_ms) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    int total = n;                                                   // everything heard in the last minute
    if (n > 20) n = 20;                                              // but only the 20 freshest travel in one frame
    ss_cbor_map(w, 3);
    ss_cbor_kv_bool(w, "canned", false);
    ss_cbor_kv_uint(w, "total", total);
    ss_cbor_text(w, "devs");
    ss_cbor_array(w, n);
    for (int i = 0; i < n; i++) {
        const BleRec& r = snap[order[i]];
        char mac[20];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
        ss_cbor_map(w, 5 + (r.has_company ? 1 : 0));
        ss_cbor_kv_text(w, "mac", mac);
        ss_cbor_kv_text(w, "name", r.name);
        ss_cbor_kv_int(w, "rssi", r.rssi);
        ss_cbor_kv_uint(w, "atype", r.addr_type);
        ss_cbor_kv_uint(w, "age_ms", now - r.last_ms);
        if (r.has_company) ss_cbor_kv_uint(w, "company", r.company);
    }
    return !w->err;
}

// ---- per-advert detection --------------------------------------------------------------------------------------
static void on_advert(const struct ble_gap_disc_desc* disc) {
    STAT_INC(g_stats.ble_adv);
    s_adv_count = s_adv_count + 1;

    static Adv a;                                        // NimBLE host task only; keep off its stack
    a = Adv();
    for (int i = 0; i < 6; i++) a.mac[i] = disc->addr.val[5 - i];     // LSB first on the air -> MSB first
    a.addr_type = disc->addr.type;
    a.rssi = disc->rssi;
    a.evtype = disc->event_type;
    parse_adv(disc->data, disc->length_data, a);
    dev_note(a);
    focus_note_ble(a.mac, disc->rssi, disc->data, disc->length_data);   // no-op unless focused on this device

    int confidence = 0, best = 0;
    const char* rule = nullptr;
    const char* category = nullptr;
    for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
        int w = match_rule(a, SIGNATURES[i]);
        if (w > 0) {
            confidence += w;
            if (w > best) { best = w; rule = SIGNATURES[i].name; category = SIGNATURES[i].category; }
        }
    }

    // Protocol detectors (upstream order): drone, AirTag, tracker network, Penguin
    const char* net = ble_tracker_network(a);
    bool drone = false, decoded = false;
    if (ble_decode_remote_id(a, &decoded)) {
        drone = true;
        confidence += W_DRONE;
        if (W_DRONE > best) { best = W_DRONE; rule = "Remote ID Drone"; category = "Drone"; }
    } else if (ble_is_airtag(a)) {
        confidence += W_TRACKER;
        if (W_TRACKER > best) { best = W_TRACKER; rule = "Apple Find My Tracker"; category = "Tracker"; }
    } else if (net) {
        confidence += W_TRACKER;
        if (W_TRACKER > best) { best = W_TRACKER; rule = net; category = "Tracker"; }
    } else if (ble_penguin(a)) {
        confidence += W_PENGUIN;
        if (W_PENGUIN > best) { best = W_PENGUIN; rule = "Flock Battery (Penguin)"; category = "Flock Safety"; }
    }

    capture_note_ble(a.mac, disc->addr.type, disc->event_type, disc->rssi, disc->data, disc->length_data,
                     disc->direct_addr.val, disc->direct_addr.type, rule, confidence, best);   // no-op unless capturing this device

    if (!rule) return;
    STAT_INC(g_stats.ble_hit);
    ThreatType t = threat_from_category(category);
    if (t == ThreatType::UNKNOWN || !type_enabled(t, g_cfg)) { STAT_INC(g_stats.ble_drop); return; }

    static Detection d;
    d = Detection();
    d.type = t;
    d.timestamp_ms = ms_now();
    d.rssi = disc->rssi;
    d.channel = 0;                                       // 0 = BLE (the advertising channel is not reported)
    d.band = 0;
    d.proto = PROTO_BLE;
    memcpy(d.mac, a.mac, 6);
    snprintf(d.name, sizeof(d.name), "%s", rule);
    capture_fields(a, d);
    if (drone && decoded) odid_apply(d, s_bleUas);
    post_detection(d, confidence, best);
}

// ---- NimBLE host --------------------------------------------------------------------------------------------------
static int gap_cb(struct ble_gap_event* ev, void*);

static void start_scan(void) {
    if (!s_synced || !s_enabled || s_paused) return;
    struct ble_gap_disc_params dp = {};
    dp.itvl = BLE_GAP_SCAN_ITVL_MS(100);                 // 100 ms interval,
    dp.window = BLE_GAP_SCAN_WIN_MS(s_duty);             // 100 ms interval x duty %: the rest of the time is Wi-Fi's
    dp.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    dp.limited = 0;
    dp.passive = 1;                                      // PASSIVE: never sends a scan request
    dp.filter_duplicates = 0;                            // keep every advert so RSSI keeps updating
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
    else ESP_LOGI(TAG, "passive scan started (100/%d ms)", (int)s_duty);
}

static int gap_cb(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_enabled) on_advert(&ev->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();                                    // restarts only if still wanted
        break;
    default: break;
    }
    return 0;
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced");
    start_scan();
}

static void on_reset(int reason) { s_synced = false; ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason); }

static void host_task(void*) {
    nimble_port_run();                                   // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

bool radio_ble_enabled(void) { return s_enabled; }

void radio_ble_set_duty(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    if (pct == s_duty) return;
    s_duty = (uint8_t)pct;
    if (!s_synced || !s_enabled || s_paused) return;
    ble_gap_disc_cancel();                               // restart with the new window (still passive)
    start_scan();
}

void radio_ble_set_paused(bool paused) {
    if (paused == s_paused) return;
    s_paused = paused;
    if (!s_synced || !s_enabled) return;
    if (paused) ble_gap_disc_cancel();
    else start_scan();
}

void radio_ble_set_enabled(bool on) {
    if (on == s_enabled) return;
    s_enabled = on;
    if (!s_synced) return;
    if (on) start_scan();
    else { ble_gap_disc_cancel(); ESP_LOGW(TAG, "BLE scan OFF (charge mode)"); }
}

void radio_ble_start(bool enabled) {
    s_enabled = enabled;
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(e)); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE started, scan %s", enabled ? "ON" : "OFF (charge mode)");
}

void radio_ble_log_stats(void) {
    static uint32_t last_adv = 0;
    uint32_t a = s_adv_count;
    uint32_t now = ms_now(), n = 0;
    portENTER_CRITICAL(&s_dev_mux);
    for (int i = 0; i < MAX_BLE; i++) if (s_devs[i].used && now - s_devs[i].last_ms < 60000) n++;
    portEXIT_CRITICAL(&s_dev_mux);
    ESP_LOGI(TAG, "scan=%s adv=%u (+%u) devs=%u hits=%u drops=%u", s_enabled ? "ON" : "OFF", (unsigned)a,
             (unsigned)(a - last_adv), (unsigned)n, (unsigned)g_stats.ble_hit, (unsigned)g_stats.ble_drop);
    last_adv = a;
}
