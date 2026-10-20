// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep CYD — control plane + HMI, split architecture M2.
// Boots SILENT_RX, brings up the UART link to the XIAO C5, runs the HELLO
// handshake, pushes the (authoritative) mode, then heartbeats GET_STATUS.
//
// Serial-monitor dev console (USB, 115200) exercises the M2 acceptance checks:
//   h HELLO   s GET_STATUS   w SCAN_WIFI   b SCAN_BLE   l LIST_DETECTIONS
//   g GATT_CONNECT_READ w/o token   a TX_SLOT_A w/o token
//   u unknown opcode   x bad CRC   v bad version   ? help
// It cannot change the mode and cannot mint tokens: both need the touch-confirm
// flow that arrives in M4/M6.
#include <Arduino.h>
#include "board_pins.h"
#include "touch.h"
#include "ss_protocol.h"
#include "ss_capabilities.h"
#include "ss_cbor.h"
#include "ss_frame.h"
#include "ss_token.h"
#include "mode.h"
#include "uart_link.h"
#include "selftest.h"
#include "status_ui.h"
#include "model.h"
#include "theme.h"
#include "ui.h"

// ---- pins (LOCKED in BUILD_MANIFEST.md section 2) ----------------------------
#define LINK_RX_PIN 27   // CYD RX  <- C5 TX (GPIO9, pad D9)
#define LINK_TX_PIN 22   // CYD TX  -> C5 RX (GPIO10, pad D10)

static UartLink g_link;
static C5Status c5;
static StatusView view;
static SelfTestResult st;

static uint8_t g_secret[SS_SESSION_SECRET_LEN];   // session secret for this boot's HELLO
static uint8_t g_pending_secret[SS_SESSION_SECRET_LEN];
static bool    g_hello_inflight = false;
static uint32_t g_next_hello_ms = 0;
static uint32_t g_next_hb_ms = 0;
static uint8_t  g_hb_misses = 0;

static void set_last(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(view.last, sizeof(view.last), fmt, ap);
    va_end(ap);
    Serial.printf("[last] %s\n", view.last);
}

static const char* err_name(uint8_t e) {
    switch (e) {
        case SS_OK: return "OK";
        case SS_ERR_VERSION: return "ERR_VERSION";
        case SS_ERR_CRC: return "ERR_CRC";
        case SS_ERR_BADFRAME: return "ERR_BADFRAME";
        case SS_ERR_OPCODE: return "ERR_OPCODE";
        case SS_ERR_MODE: return "ERR_MODE";
        case SS_ERR_TOKEN: return "ERR_TOKEN";
        case SS_ERR_BUSY: return "ERR_BUSY";
        case SS_ERR_RADIO: return "ERR_RADIO";
        case SS_ERR_UNIMPL: return "ERR_UNIMPL";
        default: return "ERR_?";
    }
}

static const char* op_name(uint8_t op) {
    switch (op) {
        case SS_OP_HELLO: return "HELLO";
        case SS_OP_SET_MODE: return "SET_MODE";
        case SS_OP_GET_STATUS: return "GET_STATUS";
        case SS_OP_SET_POWER: return "SET_POWER";
        default: { const ss_cap_t* c = ss_cap_find((ss_opcode_t)op); return c ? c->id : "op?"; }
    }
}

// ---- protocol actions ---------------------------------------------------------
static void send_hello() {
    esp_fill_random(g_pending_secret, sizeof(g_pending_secret));
    uint8_t buf[96];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 3);
    ss_cbor_kv_uint(&w, "ver", SS_PROTO_VERSION);
    ss_cbor_kv_bytes(&w, "secret", g_pending_secret, sizeof(g_pending_secret));
    ss_cbor_kv_uint(&w, "t_ms", millis());
    if (!w.err && g_link.send_req(SS_OP_HELLO, nullptr, buf, w.len) >= 0) g_hello_inflight = true;
}

static void send_set_mode() {
    uint8_t buf[16];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_uint(&w, "mode", (uint32_t)mode_get());
    g_link.send_req(SS_OP_SET_MODE, nullptr, buf, w.len);
}

static void send_simple(uint8_t op) { g_link.send_req(op, nullptr, nullptr, 0); }

static void send_set_power(bool charge) {
    uint8_t buf[16];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_bool(&w, "charge", charge);
    g_link.send_req(SS_OP_SET_POWER, nullptr, buf, w.len);
}

static void send_set_config() {
    uint8_t buf[64];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 5);
    ss_cbor_kv_bool(&w, "drone", g_ui_set.detect[0]);
    ss_cbor_kv_bool(&w, "tracker", g_ui_set.detect[1]);
    ss_cbor_kv_bool(&w, "camera", g_ui_set.detect[2]);
    ss_cbor_kv_bool(&w, "bodycam", g_ui_set.detect[3]);
    ss_cbor_kv_bool(&w, "attack", g_ui_set.detect[4]);
    g_link.send_req(SS_OP_SET_CONFIG, nullptr, buf, w.len);
}

static void send_list(uint32_t offset) {
    uint8_t buf[16];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_uint(&w, "offset", offset);
    g_link.send_req(SS_OP_LIST_DETECTIONS, nullptr, buf, w.len);
}

static bool s_print_aps = false, s_print_list = false, s_print_ble = false, s_print_profile = false;

static void send_profile(const char* mac) {
    uint8_t buf[48];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_text(&w, "mac", mac);
    g_link.send_req(SS_OP_GET_DEVICE_PROFILE, nullptr, buf, w.len);
}   // console 'w' / 'l' want the text dump

// ---- focus mode (the C5 parks on the target shown on INFO) ----------------------------------
// Focus is a lease on the C5: renewed every FOCUS_REFRESH_MS while INFO shows a detection, released
// when the screen moves on. If this firmware or the link dies, the C5 lets it lapse by itself.
#define FOCUS_REFRESH_MS 5000
static char     s_focus_mac[18] = {};
static bool     s_focus_on = false;          // we asked the C5 to focus s_focus_mac
static bool     s_focus_acked = false;       // ... and it said yes
static uint32_t s_focus_sent_ms = 0, s_focus_fail_ms = 0, s_view_ms = 0;
static bool     s_fv_inflight = false;       // a {view:"focus"} request is outstanding (shares the opcode with the profile)

static void send_focus(const char* mac, bool on) {
    uint8_t buf[64];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    const Det* d = on ? model_find(mac) : nullptr;
    uint8_t ch = d ? d->ch : 0;
    size_t pairs = 2 + (on ? 1 : 0) + (on && ch ? 1 : 0);
    ss_cbor_map(&w, pairs);
    ss_cbor_kv_bool(&w, "on", on);
    ss_cbor_kv_text(&w, "mac", mac);
    if (on) ss_cbor_kv_text(&w, "radio", (d && d->radio == 1) ? "ble" : "wifi");   // BLE+Wi-Fi targets: Wi-Fi
    if (on && ch) ss_cbor_kv_uint(&w, "ch", ch);
    g_link.send_req(SS_OP_FOCUS_TARGET, nullptr, buf, w.len);
}

static void send_focus_view() {
    uint8_t buf[32];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_text(&w, "view", "focus");
    s_fv_inflight = true;
    g_link.send_req(SS_OP_GET_DEVICE_PROFILE, nullptr, buf, w.len);
}

// Console test hooks (they bypass the INFO screen; the C5's own 15 s lease then ends the focus)
static bool s_print_fv = false;
static char s_dbg_mac[18] = {};
static void dbg_focus(const char* mac, const char* radio, uint8_t ch) {
    uint8_t buf[64];
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, ch ? 4 : 3);
    ss_cbor_kv_bool(&w, "on", true);
    ss_cbor_kv_text(&w, "mac", mac);
    ss_cbor_kv_text(&w, "radio", radio);
    if (ch) ss_cbor_kv_uint(&w, "ch", ch);
    strncpy(s_dbg_mac, mac, sizeof(s_dbg_mac) - 1);
    Serial.printf("[focus] request %s %s ch%u\n", mac, radio, (unsigned)ch);
    g_link.send_req(SS_OP_FOCUS_TARGET, nullptr, buf, w.len);
}

static void focus_reset() { s_focus_on = false; s_focus_acked = false; g_focus_keep[0] = 0; model_focus_clear(); }

static void focus_service(uint32_t now) {
    char mac[18];
    bool want = ui_wants_focus(mac);
    if (g_link.busy()) return;
    if (want) {
        bool changed = !s_focus_on || strcmp(mac, s_focus_mac) != 0;
        if ((changed || (int32_t)(now - s_focus_sent_ms) >= FOCUS_REFRESH_MS) && (int32_t)(now - s_focus_fail_ms) >= 2000) {
            if (changed) { model_focus_clear(); s_focus_acked = false; }
            strncpy(s_focus_mac, mac, sizeof(s_focus_mac) - 1);
            strncpy(g_focus_keep, mac, sizeof(g_focus_keep) - 1);
            s_focus_on = true;
            s_focus_sent_ms = now;
            send_focus(mac, true);
            return;
        }
        int every = ui_focus_view_period();
        if (s_focus_acked && every && (int32_t)(now - s_view_ms) >= every) { s_view_ms = now; send_focus_view(); }
    } else if (s_focus_on) {
        send_focus(s_focus_mac, false);
        focus_reset();
    }
}

// Charge mode mirrors whatever the C5 reports (it owns the state: it measures the battery and
// enforces the 3.4 V forced entry / 4.0 V forced exit). The screen follows.
static bool s_charge_ui = false;
static bool s_screen_on = true;              // charge mode: panel awake?
static uint32_t s_wake_until = 0;            // ... until this millis()
static uint32_t s_cut_deadline = 0;          // millis() when the C5 cuts our supply (0 = none)
static bool s_sim_touch = false;             // console 't': pretend the screen was tapped
static void apply_power_ui() {
    bool want = (c5.power != SS_POWER_NORMAL);
    if (want == s_charge_ui) return;
    s_charge_ui = want;
    status_ui_set_charge(want);
    if (!want) ui_invalidate();
    s_screen_on = true;
    s_wake_until = millis() + SS_CHARGE_WAKE_MS;     // show the voltage briefly, then the panel goes dark
    g_next_hb_ms = millis() + (want ? 2000 : 200);   // re-time the heartbeat for the new mode
    Serial.printf("[power] %s\n", want ? (c5.power == SS_POWER_CHARGE_FORCED ? "CHARGE MODE (forced, low battery)" : "CHARGE MODE (user)") : "normal");
}

// ---- decoders for the real scan data (printed to serial; the list UI is M4/M5) ----
static void print_aps(const uint8_t* cb, size_t n) {
    ss_cbor_val_t arr;
    if (!ss_cbor_map_get(cb, n, "aps", &arr) || arr.type != SS_CBOR_ARRAY) return;
    const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
    unsigned n2 = 0, n5 = 0;
    Serial.printf("[aps] %u access points\n", (unsigned)arr.n);
    for (size_t i = 0; i < arr.n; i++) {
        ss_cbor_val_t ssid, ch, band, rssi;
        const uint8_t* item = p;
        bool ok = ss_cbor_map_get(item, end - item, "ssid", &ssid) && ss_cbor_map_get(item, end - item, "ch", &ch) &&
                  ss_cbor_map_get(item, end - item, "band", &band) && ss_cbor_map_get(item, end - item, "rssi", &rssi);
        if (ok) {
            (band.u == 5 ? n5 : n2)++;
            Serial.printf("[aps]  %uG ch%-3u %4d dBm  %.*s\n", (unsigned)band.u, (unsigned)ch.u, (int)rssi.i,
                          (int)ssid.n, ssid.ptr ? (const char*)ssid.ptr : "");
        }
        if (!ss_cbor_skip(&p, end)) break;
    }
    set_last("APs: %u on 2.4G, %u on 5G", n2, n5);
}

static void print_ble(const uint8_t* cb, size_t n) {
    ss_cbor_val_t arr;
    if (!ss_cbor_map_get(cb, n, "devs", &arr) || arr.type != SS_CBOR_ARRAY) return;
    const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
    Serial.printf("[ble] %u devices\n", (unsigned)arr.n);
    for (size_t i = 0; i < arr.n; i++) {
        ss_cbor_val_t mac, name, rssi, company;
        const uint8_t* item = p;
        if (ss_cbor_map_get(item, end - item, "mac", &mac) && ss_cbor_map_get(item, end - item, "rssi", &rssi)) {
            bool hn = ss_cbor_map_get(item, end - item, "name", &name) && name.n > 0;
            bool hc = ss_cbor_map_get(item, end - item, "company", &company);
            Serial.printf("[ble]  %4d dBm  %.*s  %.*s", (int)rssi.i, (int)mac.n, (const char*)mac.ptr, hn ? (int)name.n : 0, hn ? (const char*)name.ptr : "");
            if (hc) Serial.printf("  co=0x%04X", (unsigned)company.u);
            Serial.println();
        }
        if (!ss_cbor_skip(&p, end)) break;
    }
}

static void print_list(const uint8_t* cb, size_t n) {
    uint32_t total = 0;
    ss_cbor_get_uint(cb, n, "total", &total);
    ss_cbor_val_t arr;
    Serial.printf("[list] %u detections\n", (unsigned)total);
    if (!ss_cbor_map_get(cb, n, "items", &arr) || arr.type != SS_CBOR_ARRAY) return;
    const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
    for (size_t i = 0; i < arr.n; i++) {
        ss_cbor_val_t name, rssi, band, ch;
        const uint8_t* item = p;
        if (ss_cbor_map_get(item, end - item, "name", &name) && ss_cbor_map_get(item, end - item, "rssi", &rssi) &&
            ss_cbor_map_get(item, end - item, "band", &band) && ss_cbor_map_get(item, end - item, "ch", &ch))
            Serial.printf("[list]  %dG ch%-3u %4d dBm  %.*s\n", (int)band.u, (unsigned)ch.u, (int)rssi.i,
                          (int)name.n, (const char*)name.ptr);
        if (!ss_cbor_skip(&p, end)) break;
    }
    set_last("list: %u detections", (unsigned)total);
}

static void link_down(const char* why) {
    if (c5.link_up) set_last("link down: %s", why);
    c5.link_up = false; c5.have = false; c5.fw[0] = 0;
    g_hello_inflight = false;
    g_hb_misses = 0;
    g_next_hello_ms = millis() + 500;
}

// ---- link callbacks -----------------------------------------------------------
static void handle_hello_resp(uint8_t status, const uint8_t* cb, size_t n) {
    g_hello_inflight = false;
    if (status != SS_OK) { set_last("HELLO -> %s", err_name(status)); g_next_hello_ms = millis() + 1500; return; }
    uint32_t ver = 0;
    ss_cbor_val_t fw;
    if (!ss_cbor_get_uint(cb, n, "ver", &ver) || ver != SS_PROTO_VERSION) {
        set_last("HELLO: version mismatch"); g_next_hello_ms = millis() + 1500; return;
    }
    memcpy(g_secret, g_pending_secret, sizeof(g_secret));
    c5.link_up = true;
    c5.fw[0] = 0;
    if (ss_cbor_map_get(cb, n, "fw", &fw) && fw.type == SS_CBOR_TEXT) {
        size_t m = fw.n < sizeof(c5.fw) - 1 ? fw.n : sizeof(c5.fw) - 1;
        memcpy(c5.fw, fw.ptr, m); c5.fw[m] = 0;
    }
    g_hb_misses = 0;
    g_next_hb_ms = millis() + 200;
    set_last("HELLO ok, fw %s", c5.fw);
    send_set_mode();                  // push the authoritative mode right away
    send_simple(SS_OP_SUBSCRIBE_EVENTS);   // stream detections as EVENTs
    send_set_config();                // the C5 keeps no settings of its own: push the saved toggles
    model_clear();
}

static void handle_status_resp(const uint8_t* cb, size_t n) {
    uint32_t u = 0; bool b = false;
    c5.have = true;
    c5.mode = ss_cbor_get_uint(cb, n, "mode", &u) ? (uint8_t)u : 0;
    c5.shadow_valid = ss_cbor_get_bool(cb, n, "valid", &b) && b;
    c5.up_ms = ss_cbor_get_uint(cb, n, "up_ms", &u) ? u : 0;
    c5.batt_mv = ss_cbor_get_uint(cb, n, "batt_mv", &u) ? (uint16_t)u : 0;
    c5.batt_low = ss_cbor_get_bool(cb, n, "batt_low", &b) && b;
    c5.power = ss_cbor_get_uint(cb, n, "power", &u) ? (uint8_t)u : 0;
    c5.dets = ss_cbor_get_uint(cb, n, "dets", &u) ? (uint16_t)u : 0;
    c5.cut_s = ss_cbor_get_uint(cb, n, "cut_s", &u) ? u : 0;
    s_cut_deadline = c5.cut_s ? millis() + c5.cut_s * 1000UL : 0;
    apply_power_ui();
    g_hb_misses = 0;
    // The C5 lost its shadow (it rebooted, or it went stale): re-handshake.
    if (!c5.shadow_valid) link_down("C5 shadow invalid");
    else if (c5.mode != (uint8_t)mode_get()) send_set_mode();
}

static void on_resp(uint8_t op, uint8_t status, const uint8_t* cb, size_t n, uint32_t rtt) {
    if (op == SS_OP_HELLO) { handle_hello_resp(status, cb, n); return; }
    if (op == SS_OP_GET_STATUS && status == SS_OK) { handle_status_resp(cb, n); return; }
    if (op == SS_OP_SET_MODE) {
        if (status != SS_OK) set_last("SET_MODE -> %s", err_name(status));
        return;
    }
    if (op == SS_OP_SET_POWER) {
        uint32_t u = 0;
        if (status == SS_OK || status == SS_ERR_LOCKED) {
            if (ss_cbor_get_uint(cb, n, "power", &u)) { c5.power = (uint8_t)u; apply_power_ui(); }
        }
        if (status == SS_ERR_LOCKED) set_last("charge locked: battery must exceed 4.00 V");
        else if (status != SS_OK) set_last("SET_POWER -> %s", err_name(status));
        return;
    }
    if (op == SS_OP_SUBSCRIBE_EVENTS) { if (status != SS_OK) set_last("SUBSCRIBE -> %s", err_name(status)); return; }
    if (op == SS_OP_SET_CONFIG) { if (status != SS_OK && status != SS_ERR_OPCODE) set_last("SET_CONFIG -> %s", err_name(status)); return; }
    if (op == SS_OP_SCAN_WIFI && status == SS_OK) {
        model_ingest_aps(cb, n);
        if (s_print_aps) { s_print_aps = false; print_aps(cb, n); }
        return;
    }
    if (op == SS_OP_FOCUS_TARGET) {
        if (s_dbg_mac[0]) { Serial.printf("[focus] reply status=%u (%u B)\n", status, (unsigned)n); }
        if (status == SS_OK) { s_focus_acked = true; s_view_ms = 0; }
        else if (s_focus_on) {                                  // refused: show why, retry in 2 s
            set_last("focus -> %s", err_name(status));
            s_focus_on = false; s_focus_acked = false; s_focus_fail_ms = millis();
        }
        return;
    }
    if (op == SS_OP_GET_DEVICE_PROFILE && s_fv_inflight) {      // the focus view, not a detection profile
        s_fv_inflight = false;
        if (status == SS_OK) model_ingest_focus(cb, n);
        else { s_focus_acked = false; s_focus_on = false; }     // C5 is no longer focused: re-engage
        if (s_print_fv) {
            s_print_fv = false;
            if (status != SS_OK) Serial.printf("[focus] view: not focused (status %u)\n", status);
            else {
                Serial.printf("[focus] view %s %s ch%u rssi=%d n=%u beacons=%u presp=%u preq=%u omgmt=%u data=%u advs=%u adv_int=%u ht=%u iemap=%d vie=%u raw=%u peers=%u hunt_ever=%d hunt_rssi=%d\n",
                              g_fv.mac, g_fv.wifi ? "wifi" : "ble", g_fv.ch, g_fv.rssi, (unsigned)g_fv.n, (unsigned)g_fv.beacons, (unsigned)g_fv.presp,
                              (unsigned)g_fv.preq, (unsigned)g_fv.omgmt, (unsigned)g_fv.data, (unsigned)g_fv.advs, (unsigned)g_fv.adv_int,
                              g_fv.ht, g_fv.has_iemap, g_fv.nvie, g_fv.raw_len, g_fv.npeers, g_hunt.ever, g_hunt.rssi);
                for (int i = 0; i < g_fv.npeers; i++) {
                    const FvPeer& p = g_fv.peers[i];
                    Serial.printf("[focus]   peer %s role=%u rssi=%d >%u <%u bytes %u/%u qos=%u\n", p.mac, p.role, p.rssi, (unsigned)p.tof,
                                  (unsigned)p.frf, (unsigned)p.tob, (unsigned)p.frb, (unsigned)p.qos);
                }
            }
        }
        return;
    }
    if (op == SS_OP_GET_DEVICE_PROFILE) {
        if (status == SS_OK) {
            model_ingest_profile(cb, n);
            if (s_print_profile) {
                s_print_profile = false;
                Serial.printf("[profile] %s atype=%u rid=%d uas=%s op=%s lat=%.5f lon=%.5f h=%.1fm hdg=%d\n", g_profile.mac,
                              g_profile.atype, g_profile.rid, g_profile.uas, g_profile.op, g_profile.lat_e6 / 1e6,
                              g_profile.lon_e6 / 1e6, g_profile.h_dm / 10.0, (int)g_profile.hdg);
            }
        } else if (s_print_profile) { s_print_profile = false; Serial.println("[profile] none (no such detection)"); }
        return;
    }
    if (op == SS_OP_SCAN_BLE && status == SS_OK) {
        model_ingest_ble(cb, n);
        if (s_print_ble) { s_print_ble = false; print_ble(cb, n); }
        return;
    }
    if (op == SS_OP_LIST_DETECTIONS && status == SS_OK) {
        uint16_t next = model_ingest_list(cb, n);      // 0 when the sweep is complete
        if (next) send_list(next);
        if (s_print_list && !next) { s_print_list = false; print_list(cb, n); }
        return;
    }
    // Everything else is a dev-console probe: report what came back
    if (status == SS_OK) {
        set_last("%s ok (%u B, %ums)", op_name(op), (unsigned)n, (unsigned)rtt);
        Serial.printf("[resp] %s: %u bytes of CBOR\n", op_name(op), (unsigned)n);
    } else {
        set_last("%s -> %s", op_name(op), err_name(status));
    }
}

static void on_nack(uint8_t err, uint8_t op) {
    set_last("NACK %s%s%s", err_name(err), op ? " for " : "", op ? op_name(op) : "");
}

static void on_event(uint8_t kind, const uint8_t* cb, size_t n) {
    if (kind == SS_EVT_STATUS) {
        // Pushed by the C5 on a power change (and every 10 s): react at once, not at the next heartbeat
        uint32_t u = 0; bool b = false;
        if (ss_cbor_get_uint(cb, n, "batt_mv", &u)) c5.batt_mv = (uint16_t)u;
        if (ss_cbor_get_bool(cb, n, "batt_low", &b)) c5.batt_low = b;
        if (ss_cbor_get_uint(cb, n, "cut_s", &u)) { c5.cut_s = u; s_cut_deadline = u ? millis() + u * 1000UL : 0; }
        if (ss_cbor_get_uint(cb, n, "power", &u)) { c5.power = (uint8_t)u; apply_power_ui(); }
        return;
    }
    if (kind == SS_EVT_RSSI_TICK) { model_ingest_tick(cb, n); return; }
    if (kind == SS_EVT_DETECTION) {
        ss_cbor_val_t name, rssi, band, ch, mac;
        bool isnew = false, alert = false;
        ss_cbor_get_bool(cb, n, "new", &isnew);
        ss_cbor_get_bool(cb, n, "alert", &alert);
        bool ev_new = false, ev_alert = false;
        int slot = model_ingest_det(cb, n, &ev_new, &ev_alert);
        if (slot >= 0 && ev_alert && !g_model.dets[slot].muted) ui_alert(g_model.dets[slot].type);
        if (ss_cbor_map_get(cb, n, "name", &name) && ss_cbor_map_get(cb, n, "rssi", &rssi) &&
            ss_cbor_map_get(cb, n, "band", &band) && ss_cbor_map_get(cb, n, "ch", &ch) &&
            ss_cbor_map_get(cb, n, "mac", &mac)) {
            Serial.printf("[det] %s%s %dG ch%u %d dBm %.*s %.*s\n", isnew ? "NEW " : "", alert ? "ALERT" : "",
                          (int)band.u, (unsigned)ch.u, (int)rssi.i, (int)mac.n, (const char*)mac.ptr,
                          (int)name.n, (const char*)name.ptr);
            set_last("%s%.*s %ddBm", alert ? "ALERT " : "", (int)(name.n > 24 ? 24 : name.n),
                     (const char*)name.ptr, (int)rssi.i);
        }
        return;
    }
    Serial.printf("[event] kind=%u (%u B)\n", kind, (unsigned)n);
}

static void on_timeout(uint8_t op) {
    if (op == SS_OP_HELLO) { g_hello_inflight = false; g_next_hello_ms = millis() + 1000; return; }
    if (op == SS_OP_GET_STATUS) {
        if (++g_hb_misses >= 3) link_down("heartbeat lost");
        return;
    }
    if (op == SS_OP_GET_DEVICE_PROFILE) s_fv_inflight = false;
    set_last("%s timed out", op_name(op));
}

// ---- dev console --------------------------------------------------------------
static void console() {
    while (Serial.available()) {
        int ch = Serial.read();
        uint8_t frame[SS_FRAME_OVERHEAD + 32];
        switch (ch) {
        case 'h': send_hello(); break;
        case 's': send_simple(SS_OP_GET_STATUS); break;
        case 'w': s_print_aps = true; send_simple(SS_OP_SCAN_WIFI); break;
        case 'b': s_print_ble = true; send_simple(SS_OP_SCAN_BLE); break;
        case 'l': s_print_list = true; send_list(0); break;
        case 'g': send_simple(SS_OP_GATT_CONNECT_READ); break;     // gated: expect ERR_MODE
        case 'a': send_simple(SS_OP_TX_SLOT_A); break;             // gated: expect ERR_MODE
        case 'c': if (c5.link_up) send_set_power(true);  else set_last("no link: cannot enter charge mode"); break;
        case 'e': if (c5.link_up) send_set_power(false); break;
        case 't': s_sim_touch = true; break;                       // simulate a screen tap
        case 'p': {                                                // profile of the first listed detection
            int i = -1;
            for (int k = 0; k < g_model.ndets; k++) if (g_model.dets[k].used && g_model.dets[k].type != T_UNKNOWN) { i = k; break; }
            if (i < 0) Serial.println("[profile] no detections yet");
            else { s_print_profile = true; send_profile(g_model.dets[i].mac); }
            break;
        }
        case 'f': {                                                // focus the first listed detection
            int i = -1;
            for (int k = 0; k < g_model.ndets; k++) if (g_model.dets[k].used && g_model.dets[k].type != T_UNKNOWN) { i = k; break; }
            if (i < 0) Serial.println("[focus] no detections yet");
            else dbg_focus(g_model.dets[i].mac, g_model.dets[i].radio == 1 ? "ble" : "wifi", g_model.dets[i].ch);
            break;
        }
        case 'F': {                                                // focus the strongest access point (Wi-Fi path)
            int best = -1;
            for (int k = 0; k < g_model.naps; k++) if (g_model.aps[k].used && (best < 0 || g_model.aps[k].rssi > g_model.aps[best].rssi)) best = k;
            if (best < 0) Serial.println("[focus] no APs yet (press w first)");
            else dbg_focus(g_model.aps[best].mac, "wifi", g_model.aps[best].ch);
            break;
        }
        case 'r': {                                                // release focus
            uint8_t buf[48]; ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
            ss_cbor_map(&w, 1); ss_cbor_kv_bool(&w, "on", false);
            g_link.send_req(SS_OP_FOCUS_TARGET, nullptr, buf, w.len);
            Serial.println("[focus] release requested");
            break;
        }
        case 'k': s_print_fv = true; send_focus_view(); break;     // print the focus view
        case 'u': send_simple(0x7E); break;                        // unknown: expect ERR_OPCODE
        case 'x': {                                                // corrupt CRC: expect NACK ERR_CRC
            size_t n = g_link.build_req_frame(frame, sizeof(frame), SS_OP_GET_STATUS, nullptr, 0);
            if (n) { frame[n - 1] ^= 0xFF; g_link.send_raw(frame, n); }
            break;
        }
        case 'v': {                                                // wrong version: expect NACK ERR_VERSION
            size_t n = g_link.build_req_frame(frame, sizeof(frame), SS_OP_GET_STATUS, nullptr, 0);
            if (n) { frame[1] = 0x7F; g_link.send_raw(frame, n); }
            break;
        }
        case '?':
            Serial.println("h HELLO  s STATUS  w SCAN_WIFI  b SCAN_BLE  l LIST  g GATT(no token)  "
                           "a TX_SLOT_A(no token)  u unknown-op  x bad-CRC  v bad-version  c charge-on  e charge-off  t tap  p profile  f focus-first  F focus-AP  r release  k focus-view");
            break;
        default: break;
        }
    }
}

// ---- Arduino ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[SignalSweep CYD split M3a] boot, mode RX-ONLY");
    touch_init();

    status_ui_init();
    static const UiCallbacks cbs = { []() { if (c5.link_up) send_set_power(true); else set_last("no link: cannot enter charge mode"); },
                                     []() { if (c5.link_up) send_set_config(); } };
    ui_init(cbs);
    st = selftest_run();                      // M1 checks, on-device

    g_link.on_resp = on_resp;
    g_link.on_nack = on_nack;
    g_link.on_event = on_event;
    g_link.on_timeout = on_timeout;
    g_link.begin(Serial2, LINK_RX_PIN, LINK_TX_PIN, SS_UART_BAUD_BRINGUP);

    view.c5 = &c5;
    view.selftest_pass = st.passed;
    view.selftest_total = st.total;
    set_last(st.passed == st.total ? "boot ok, waiting for C5" : "SELFTEST FAILED");
    g_next_hello_ms = millis() + 500;
}

void loop() {
    g_link.poll();
    console();

    uint32_t now = millis();
    if (!c5.link_up) {
        if (!g_hello_inflight && !g_link.busy() && (int32_t)(now - g_next_hello_ms) >= 0) {
            send_hello();
            g_next_hello_ms = now + 1500;
        }
    } else if ((int32_t)(now - g_next_hb_ms) >= 0 && !g_link.busy()) {
        send_simple(SS_OP_GET_STATUS);
        g_next_hb_ms = now + (s_charge_ui ? SS_CHARGE_HEARTBEAT_MS : SS_HEARTBEAT_MS);
    }

    view.tx = g_link.stats.tx; view.rx = g_link.stats.rx; view.nack = g_link.stats.nack;
    view.crc_err = g_link.stats.crc_err; view.timeouts = g_link.stats.timeouts;
    view.rtt_ms = g_link.stats.last_rtt_ms;
    view.raw = g_link.stats.raw_bytes;
    static uint32_t logged = 0;
    if (now - logged >= 2000) {
        logged = now;
        Serial.printf("[link] tx=%u rx=%u raw=%u crc=%u nack=%u timeout=%u up=%d batt_mv=%u c5mode=%u\n",
            (unsigned)g_link.stats.tx, (unsigned)g_link.stats.rx, (unsigned)g_link.stats.raw_bytes,
            (unsigned)g_link.stats.crc_err, (unsigned)g_link.stats.nack,
            (unsigned)g_link.stats.timeouts, (int)c5.link_up,
            (unsigned)c5.batt_mv, (unsigned)c5.mode);
    }
    // Touch: act on the touch-down edge. Polled at ~25 Hz normally, ~10 Hz while the charge-mode
    // panel is dark (the touch controller stays on so a tap can wake the screen).
    static uint32_t touch_ms = 0;
    static bool was_touched = false;
    uint32_t touch_period = (s_charge_ui && !s_screen_on) ? 100 : 40;
    if (now - touch_ms >= touch_period) {
        touch_ms = now;
        int tx = 0, ty = 0;
        bool t = touch_get(tx, ty);
        bool tap = (t && !was_touched) || s_sim_touch;
        s_sim_touch = false;
        was_touched = t;
        if (tap) {
            if (s_charge_ui && !s_screen_on) {
                // First tap only wakes the panel (and refreshes the voltage); it never exits
                s_screen_on = true;
                s_wake_until = now + SS_CHARGE_WAKE_MS;
                status_ui_screen_power(true);
                if (c5.link_up) send_simple(SS_OP_GET_STATUS);
            } else if (c5.power == SS_POWER_CHARGE_USER) {
                send_set_power(false);                       // tap on the lit screen leaves a user-selected charge mode
            } else if (c5.power == SS_POWER_CHARGE_FORCED) {
                s_wake_until = now + SS_CHARGE_WAKE_MS;      // forced: a tap only keeps the screen lit
            } else {
                ui_touch(tx, ty);                            // normal mode: tabs, lists, settings
            }
        }
    }

    // Charge mode: the panel goes fully dark after SS_CHARGE_WAKE_MS without a tap
    if (s_charge_ui && s_screen_on && (int32_t)(now - s_wake_until) >= 0) {
        s_screen_on = false;
        status_ui_screen_power(false);
        Serial.println("[power] screen off (tap to wake)");
    }

    // Poll the C5 for what the screens show (events already push new detections as they happen)
    static uint32_t next_list_ms = 0, next_aps_ms = 0, next_ble_bg_ms = 0;
    if (!(c5.link_up && !s_charge_ui && c5.power == SS_POWER_NORMAL) && (s_focus_on || s_focus_acked)) focus_reset();   // the C5 ends focus on its own
    if (c5.link_up && !s_charge_ui && c5.power == SS_POWER_NORMAL) {
        focus_service(now);                                   // before the polls so engaging is never starved
        if ((int32_t)(now - next_list_ms) >= 0 && !g_link.busy()) { next_list_ms = now + 2000; send_list(0); }
        if (ui_wants_aps() && (int32_t)(now - next_aps_ms) >= 0 && !g_link.busy()) { next_aps_ms = now + 2500; send_simple(SS_OP_SCAN_WIFI); }
        char pmac[18];
        static uint32_t next_prof_ms = 0;
        if (ui_wants_profile(pmac) && (int32_t)(now - next_prof_ms) >= 0 && !g_link.busy()) { next_prof_ms = now + 2000; send_profile(pmac); }
        if (ui_wants_ble() && (int32_t)(now - next_aps_ms) >= 0 && !g_link.busy()) { next_aps_ms = now + 2500; send_simple(SS_OP_SCAN_BLE); }
        // Slow background poll on every other screen keeps the header's BLE count current
        else if (!ui_wants_ble() && (int32_t)(now - next_ble_bg_ms) >= 0 && !g_link.busy()) { next_ble_bg_ms = now + 5000; send_simple(SS_OP_SCAN_BLE); }
    }

    if (!s_charge_ui) {
        UiCtx ctx = { &c5, mode_get(), &view };
        ui_update(ctx);
    }

    static uint32_t drawn = 0;
    if (now - drawn >= 250) {
        drawn = now;
        if (s_charge_ui) {
            if (s_screen_on) {
                uint32_t left = (s_cut_deadline && (int32_t)(s_cut_deadline - now) > 0) ? (s_cut_deadline - now + 999) / 1000 : 0;
                status_ui_draw_charge(c5, left);
            }
        }
    }
    delay(s_charge_ui ? (s_screen_on ? 50 : 100) : 1);
}
