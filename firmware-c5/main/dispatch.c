// SPDX-License-Identifier: GPL-3.0-or-later
// dispatch.c â€” opcode -> handler, with the C5's own gate in front of every op.
//
// Invariants enforced here (BUILD_MANIFEST.md section 4):
//   2. Boot silent: the shadow starts invalid (treated as SILENT_RX).
//   4. Fail safe: a stale or unknown shadow refuses ACTIVE ops. The shadow goes
//      stale if the CYD is silent for SS_SHADOW_STALE_MS.
//   5. Token per op: ACTIVE ops need a valid, fresh, single-use token.
//   7. PASSIVE means passive: nothing in this file transmits RF.
// M3b scope: HELLO / SET_MODE / GET_STATUS, SCAN_WIFI (real, dual-band passive), SCAN_BLE (real,
// passive), LIST_DETECTIONS and SUBSCRIBE_EVENTS are real; everything else that passes the gate
// answers SS_ERR_UNIMPL until its milestone lands.
#include "dispatch.h"
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "ss_protocol.h"
#include "ss_capabilities.h"
#include "ss_cbor.h"
#include "ss_token.h"
#include "uart_slave.h"
#include "battery.h"
#include "tx_slots.h"
#include "detections_api.h"
#include "odid_selftest.h"
#include "power.h"

static const char* TAG = "dispatch";
#define FW_NAME "c5-m3c"

static struct {
    bool      valid;                              // a HELLO has established this shadow
    ss_mode_t mode;
    uint32_t  last_rx_ms;                         // last valid frame from the CYD
    bool      have_secret;
    uint8_t   secret[SS_SESSION_SECRET_LEN];
    int32_t   clk_offset;                         // CYD clock minus our clock, from HELLO
    uint32_t  last_evt_ms;
    bool      low_reported;
    bool      subscribed;                         // SUBSCRIBE_EVENTS: stream detections as EVENTs
} S;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Mode the gate must use right now. Latches to silent when the shadow goes stale.
static ss_mode_t effective_mode(uint32_t now) {
    uint32_t window = power_radio_allowed() ? SS_SHADOW_STALE_MS : SS_CHARGE_SHADOW_STALE_MS;
    if (S.valid && (now - S.last_rx_ms) > window) {
        S.valid = false;
        S.mode = SS_MODE_SILENT_RX;
        S.have_secret = false;
        memset(S.secret, 0, sizeof(S.secret));
        ESP_LOGW(TAG, "mode shadow went stale -> SILENT_RX, session dropped");
        det_focus_stop("link lost");
    }
    return S.valid ? S.mode : SS_MODE_SILENT_RX;
}

void dispatch_init(void) {
    memset(&S, 0, sizeof(S));
    S.mode = SS_MODE_SILENT_RX;
}

// RESP payload = [status][CBOR result]
static void send_resp(uint8_t seq, uint8_t status, const uint8_t* cbor, size_t n) {
    static uint8_t buf[SS_MAX_PAYLOAD];
    if (n > sizeof(buf) - 1) { status = SS_ERR_BUSY; n = 0; }
    buf[0] = status;
    if (n) memcpy(buf + 1, cbor, n);
    uart_slave_send(SS_T_RESP, seq, 0, buf, (uint16_t)(n + 1));
}

static void send_status_event(void) {
    uint8_t b[128];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b + 1, sizeof(b) - 1);
    uint16_t mv = battery_mv();
    uint8_t st = det_scan_state();
    ss_cbor_map(&w, 8);
    ss_cbor_kv_uint(&w, "band", (st & 0x40) ? 5 : 2);        // the Wi-Fi band being scanned right now
    ss_cbor_kv_uint(&w, "scan", st & 0x0F);                  // what the radio is doing: 1 Wi-Fi, 2 BLE, 3 both
    ss_cbor_kv_uint(&w, "lock", (st >> 4) & 0x03);                    // focus lock: 0 none, 1 Wi-Fi, 2 BLE
    ss_cbor_kv_uint(&w, "power", power_state());
    ss_cbor_kv_uint(&w, "cut_s", power_cyd_cut_in_s());
    ss_cbor_kv_uint(&w, "batt_mv", mv);
    ss_cbor_kv_bool(&w, "batt_low", mv && mv <= SS_BATT_LOW_MV);
    ss_cbor_kv_uint(&w, "mode_shadow", S.valid ? S.mode : SS_MODE_SILENT_RX);
    if (w.err) return;
    b[0] = SS_EVT_STATUS;
    uart_slave_send(SS_T_EVENT, 0, 0, b, (uint16_t)(w.len + 1));
}

// ---- control ops ------------------------------------------------------------
static void op_hello(uint8_t seq, const uint8_t* a, size_t n) {
    uint32_t ver = 0, t_ms = 0;
    uint8_t secret[SS_SESSION_SECRET_LEN];
    if (!ss_cbor_get_uint(a, n, "ver", &ver) || !ss_cbor_get_uint(a, n, "t_ms", &t_ms) ||
        !ss_cbor_get_bytes(a, n, "secret", secret, sizeof(secret))) {
        send_resp(seq, SS_ERR_BADFRAME, NULL, 0);
        return;
    }
    if (ver != SS_PROTO_VERSION) { send_resp(seq, SS_ERR_VERSION, NULL, 0); return; }

    memcpy(S.secret, secret, sizeof(S.secret));
    memset(secret, 0, sizeof(secret));
    S.have_secret = true;
    S.valid = true;
    S.mode = SS_MODE_SILENT_RX;                   // every new session starts silent
    S.clk_offset = (int32_t)(t_ms - now_ms());    // lets us judge token freshness on the CYD clock
    S.low_reported = false;
    S.subscribed = false;                         // a new session must re-subscribe
    det_focus_stop("new session");                // focus never survives a reconnect
    ss_token_reset_replay_window();

    uint8_t b[64];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    ss_cbor_map(&w, 4);
    ss_cbor_kv_uint(&w, "ver", SS_PROTO_VERSION);
    ss_cbor_kv_text(&w, "fw", FW_NAME);
    ss_cbor_kv_uint(&w, "t_ms", now_ms());
    ss_cbor_kv_uint(&w, "mode", SS_MODE_SILENT_RX);
    send_resp(seq, w.err ? SS_ERR_BUSY : SS_OK, b, w.len);
    ESP_LOGI(TAG, "HELLO ok, session started, shadow SILENT_RX");
}

static void op_set_mode(uint8_t seq, const uint8_t* a, size_t n) {
    uint32_t m = 0;
    if (!S.valid) { send_resp(seq, SS_ERR_MODE, NULL, 0); return; }   // no session: HELLO first
    if (!ss_cbor_get_uint(a, n, "mode", &m) || m > SS_MODE_TX_DETECTABLE) {
        send_resp(seq, SS_ERR_BADFRAME, NULL, 0);
        return;
    }
    S.mode = (ss_mode_t)m;
    ESP_LOGI(TAG, "SET_MODE -> %s", m ? "TX_DETECTABLE" : "SILENT_RX");
    send_resp(seq, SS_OK, NULL, 0);
}

// SET_POWER {charge:bool}: user-selected charge mode on/off. Refused (SS_ERR_LOCKED) while the
// low-battery forced charge mode is active; the reply always carries the resulting state.
static void op_set_power(uint8_t seq, const uint8_t* a, size_t n) {
    bool charge = false;
    if (!S.valid) { send_resp(seq, SS_ERR_MODE, NULL, 0); return; }   // no session: HELLO first
    if (!ss_cbor_get_bool(a, n, "charge", &charge)) { send_resp(seq, SS_ERR_BADFRAME, NULL, 0); return; }
    ss_err_t e = power_request(charge);
    uint8_t b[16];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    ss_cbor_map(&w, 1);
    ss_cbor_kv_uint(&w, "power", power_state());
    send_resp(seq, w.err ? SS_ERR_BUSY : (uint8_t)e, b, w.len);
}

// SET_CONFIG {drone,tracker,camera,bodycam,attack: bool}: which detection types are on. Missing
// keys leave a type as it was. The CYD pushes its saved settings after every HELLO.
static void op_set_config(uint8_t seq, const uint8_t* a, size_t n) {
    int v[5] = { -1, -1, -1, -1, -1 };
    static const char* K[5] = { "drone", "tracker", "camera", "bodycam", "attack" };
    for (int i = 0; i < 5; i++) { bool b; if (ss_cbor_get_bool(a, n, K[i], &b)) v[i] = b ? 1 : 0; }
    det_set_config(v[0], v[1], v[2], v[3], v[4]);
    uint32_t u;                                   // scan scheduler (optional keys; see sched.h)
    int sm = -1, sb = -1, sw = -1, sbl = -1, sbd = -1;
    if (ss_cbor_get_uint(a, n, "smode", &u)) sm = (int)u;
    if (ss_cbor_get_uint(a, n, "bpct", &u)) sb = (int)u;
    if (ss_cbor_get_uint(a, n, "wms", &u)) sw = (int)u;
    if (ss_cbor_get_uint(a, n, "bms", &u)) sbl = (int)u;
    if (ss_cbor_get_uint(a, n, "bdms", &u)) sbd = (int)u;
    if (sm >= 0 || sb >= 0 || sw >= 0 || sbl >= 0 || sbd >= 0) det_set_sched(sm, sb, sw, sbl, sbd);
    send_resp(seq, SS_OK, NULL, 0);
}

static void op_get_status(uint8_t seq) {
    uint32_t now = now_ms();
    ss_mode_t eff = effective_mode(now);          // may latch the shadow invalid first
    uint8_t b[128];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    uint16_t mv = battery_mv();
    uint8_t st = det_scan_state();
    ss_cbor_map(&w, 12);
    ss_cbor_kv_uint(&w, "band", (st & 0x40) ? 5 : 2);
    ss_cbor_kv_uint(&w, "scan", st & 0x0F);
    ss_cbor_kv_uint(&w, "lock", (st >> 4) & 0x03);
    ss_cbor_kv_uint(&w, "power", power_state());
    ss_cbor_kv_uint(&w, "cut_s", power_cyd_cut_in_s());
    ss_cbor_kv_uint(&w, "mode", eff);
    ss_cbor_kv_bool(&w, "valid", S.valid);
    ss_cbor_kv_uint(&w, "up_ms", now);
    ss_cbor_kv_uint(&w, "q", 0);
    ss_cbor_kv_uint(&w, "batt_mv", mv);
    ss_cbor_kv_bool(&w, "batt_low", mv && mv <= SS_BATT_LOW_MV);
    ss_cbor_kv_uint(&w, "dets", det_count());
    send_resp(seq, w.err ? SS_ERR_BUSY : SS_OK, b, w.len);
}

// ---- real PASSIVE results ---------------------------------------------------
static uint8_t s_big[3072];                        // scan / list reply staging (link task only)

static void op_scan_wifi(uint8_t seq) {
    ss_cbor_w_t w; ss_cbor_w_init(&w, s_big, sizeof(s_big));
    bool ok = det_encode_aps(&w);
    send_resp(seq, ok ? SS_OK : SS_ERR_BUSY, s_big, ok ? w.len : 0);
}

static void op_scan_ble(uint8_t seq) {
    ss_cbor_w_t w; ss_cbor_w_init(&w, s_big, sizeof(s_big));
    bool ok = det_encode_ble(&w);
    send_resp(seq, ok ? SS_OK : SS_ERR_BUSY, s_big, ok ? w.len : 0);
}

// GET_DEVICE_PROFILE {mac:text}: the deep passive profile of one detection (identity, signal, BLE /
// Wi-Fi fields, decoded Remote ID). SS_ERR_OPCODE-style "unknown device" is reported as SS_ERR_BUSY.
static void op_get_profile(uint8_t seq, const uint8_t* a, size_t n) {
    ss_cbor_val_t vw;
    if (ss_cbor_map_get(a, n, "view", &vw) && vw.type == SS_CBOR_TEXT && vw.n == 5 && memcmp(vw.ptr, "focus", 5) == 0) {
        ss_cbor_w_t fw; ss_cbor_w_init(&fw, s_big, sizeof(s_big));
        bool fok = det_encode_focus(&fw);                       // false: nothing is focused (lease lapsed?)
        send_resp(seq, fok ? SS_OK : SS_ERR_BUSY, s_big, fok ? fw.len : 0);
        return;
    }
    ss_cbor_val_t m;
    char mac[24];
    if (!ss_cbor_map_get(a, n, "mac", &m) || m.type != SS_CBOR_TEXT || m.n == 0 || m.n >= sizeof(mac)) {
        send_resp(seq, SS_ERR_BADFRAME, NULL, 0);
        return;
    }
    memcpy(mac, m.ptr, m.n); mac[m.n] = 0;
    ss_cbor_w_t w; ss_cbor_w_init(&w, s_big, sizeof(s_big));
    bool ok = det_encode_profile(mac, &w);
    send_resp(seq, ok ? SS_OK : SS_ERR_BUSY, s_big, ok ? w.len : 0);
}

// FOCUS_TARGET {on, mac, radio, ch}: PASSIVE. Parks the radio on one target (or releases it).
static void op_focus(uint8_t seq, const uint8_t* a, size_t n) {
    uint8_t b[48];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    int e = det_focus_request(a, n, &w);
    send_resp(seq, w.err && e == SS_OK ? SS_ERR_BUSY : (uint8_t)e, b, e == SS_OK ? w.len : 0);
}

static void op_list_detections(uint8_t seq, const uint8_t* a, size_t n) {
    uint32_t offset = 0;
    if (n) (void)ss_cbor_get_uint(a, n, "offset", &offset);   // optional paging argument
    ss_cbor_w_t w; ss_cbor_w_init(&w, s_big, sizeof(s_big));
    bool ok = det_encode_list(&w, offset);
    send_resp(seq, ok ? SS_OK : SS_ERR_BUSY, s_big, ok ? w.len : 0);
}

// ---- canned PASSIVE results (M2 only; replaced by real scans in M3) ----------
static void op_scan_wifi_canned(uint8_t seq) __attribute__((unused));
static void op_scan_wifi_canned(uint8_t seq) {
    uint8_t b[256];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    ss_cbor_map(&w, 2);
    ss_cbor_kv_bool(&w, "canned", true);
    ss_cbor_text(&w, "aps"); ss_cbor_array(&w, 2);
    ss_cbor_map(&w, 4);
    ss_cbor_kv_text(&w, "ssid", "CANNED-2G"); ss_cbor_kv_uint(&w, "ch", 6);
    ss_cbor_kv_int(&w, "rssi", -55);          ss_cbor_kv_text(&w, "band", "2g");
    ss_cbor_map(&w, 4);
    ss_cbor_kv_text(&w, "ssid", "CANNED-5G"); ss_cbor_kv_uint(&w, "ch", 36);
    ss_cbor_kv_int(&w, "rssi", -61);          ss_cbor_kv_text(&w, "band", "5g");
    send_resp(seq, w.err ? SS_ERR_BUSY : SS_OK, b, w.len);
}

static void op_scan_ble_canned(uint8_t seq) __attribute__((unused));
static void op_scan_ble_canned(uint8_t seq) {
    uint8_t b[192];
    ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
    ss_cbor_map(&w, 2);
    ss_cbor_kv_bool(&w, "canned", true);
    ss_cbor_text(&w, "devs"); ss_cbor_array(&w, 1);
    ss_cbor_map(&w, 3);
    ss_cbor_kv_text(&w, "mac", "AA:BB:CC:00:11:22");
    ss_cbor_kv_text(&w, "name", "CANNED-BLE");
    ss_cbor_kv_int(&w, "rssi", -70);
    send_resp(seq, w.err ? SS_ERR_BUSY : SS_OK, b, w.len);
}

// ---- entry ------------------------------------------------------------------
void dispatch_handle(const ss_frame_t* f) {
    uint32_t now = now_ms();
    if (f->type != SS_T_REQ) return;              // only REQs are answered
    S.last_rx_ms = now;                           // any valid frame proves the CYD is alive

    if (f->len < 1) { send_resp(f->seq, SS_ERR_BADFRAME, NULL, 0); return; }
    uint8_t op = f->payload[0];
    const uint8_t* p = f->payload + 1;
    size_t n = f->len - 1;
    const uint8_t* token = NULL;
    if (f->flags & SS_FLAG_HAS_TOKEN) {
        if (n < SS_TOKEN_WIRE_LEN) { send_resp(f->seq, SS_ERR_BADFRAME, NULL, 0); return; }
        token = p; p += SS_TOKEN_WIRE_LEN; n -= SS_TOKEN_WIRE_LEN;
    }

    // Control ops: no class, no token
    if (op == SS_OP_HELLO)      { op_hello(f->seq, p, n); return; }
    if (op == SS_OP_SET_MODE)   { op_set_mode(f->seq, p, n); return; }
    if (op == SS_OP_SET_POWER)  { op_set_power(f->seq, p, n); return; }
    if (op == SS_OP_SET_CONFIG) { op_set_config(f->seq, p, n); return; }
    if (op == SS_OP_GET_STATUS) { op_get_status(f->seq); return; }

    // Everything else is a registry capability and goes through the gate
    const ss_cap_t* c = ss_cap_find((ss_opcode_t)op);
    if (!c) { send_resp(f->seq, SS_ERR_OPCODE, NULL, 0); return; }

    bool token_valid = false;
    if (c->requires_token && token && S.have_secret) {
        // Judge freshness on the CYD's clock: our clock plus the HELLO offset.
        token_valid = ss_token_verify(token, (ss_opcode_t)op, now + (uint32_t)S.clk_offset, S.secret) == SS_OK;
    }
    ss_err_t g = ss_cap_check((ss_opcode_t)op, effective_mode(now), token_valid);
    if (g != SS_OK) {
        ESP_LOGI(TAG, "%s refused: err=%d", c->id, (int)g);
        send_resp(f->seq, (uint8_t)g, NULL, 0);
        return;
    }

    if (!power_radio_allowed() &&
        (op == SS_OP_SCAN_WIFI || op == SS_OP_SCAN_BLE || op == SS_OP_FOCUS_TARGET || c->cls != SS_CLS_PASSIVE)) {
        send_resp(f->seq, SS_ERR_BUSY, NULL, 0);   // charge mode: radio is off
        return;
    }

    switch (op) {
    case SS_OP_SCAN_WIFI:        op_scan_wifi(f->seq); return;
    case SS_OP_SCAN_BLE:         op_scan_ble(f->seq); return;
    case SS_OP_LIST_DETECTIONS:  op_list_detections(f->seq, p, n); return;
    case SS_OP_GET_DEVICE_PROFILE: op_get_profile(f->seq, p, n); return;
    case SS_OP_FOCUS_TARGET:     op_focus(f->seq, p, n); return;
    case SS_OP_SUBSCRIBE_EVENTS: S.subscribed = true; send_resp(f->seq, SS_OK, NULL, 0); return;
    case SS_OP_TX_SLOT_A: case SS_OP_TX_SLOT_B: case SS_OP_TX_SLOT_C: case SS_OP_TX_SLOT_D:
        send_resp(f->seq, (uint8_t)tx_slot_run((ss_opcode_t)op, p, n), NULL, 0);
        return;
    default:
        send_resp(f->seq, SS_ERR_UNIMPL, NULL, 0);   // gate passed, handler lands in a later milestone
        return;
    }
}

void dispatch_tick(void) {
    uint32_t now = now_ms();
    (void)effective_mode(now);                    // lets the shadow go stale even with no traffic
    det_tick();                                   // prune stale detections, periodic stats line
    // Charge mode: nothing to scan, so the link task polls twice a second instead of 50 times
    static bool idle_slow = false;
    if (idle_slow == power_radio_allowed()) {
        idle_slow = !power_radio_allowed();
        uart_slave_set_idle_ms(idle_slow ? 500 : 20);
    }
    static bool selftest_done = false;
    if (!selftest_done && now > 4000) { selftest_done = true; power_selftest(); odid_selftest(); }   // after the USB console is up
    static ss_power_t last_power = SS_POWER_NORMAL;
    static uint32_t last_pwr_ms = 0;
    if (now - last_pwr_ms >= 1000) {
        last_pwr_ms = now;
        power_tick(battery_mv());                 // forced entry at the low threshold, forced exit above 4.0 V
    }
    if (power_state() != last_power) {            // tell the CYD at once, not at the next heartbeat
        last_power = power_state();
        if (S.valid) { S.last_evt_ms = now; send_status_event(); }
    }
    static uint8_t last_scan = 0xFF;
    uint8_t scan_now = det_scan_state();
    if (scan_now != last_scan) {                  // a slice flipped or focus locked: tell the CYD at once
        last_scan = scan_now;
        if (S.valid && power_radio_allowed()) { S.last_evt_ms = now; send_status_event(); }
    }
    if (!S.valid) return;

    // Stream queued detections to the CYD (a few per tick so the link stays responsive)
    if (S.subscribed) {
        for (int k = 0; k < 3; k++) {
            uint8_t b[400];
            ss_cbor_w_t w; ss_cbor_w_init(&w, b + 1, sizeof(b) - 1);
            if (!det_pop_event(&w)) break;
            b[0] = SS_EVT_DETECTION;
            uart_slave_send(SS_T_EVENT, 0, 0, b, (uint16_t)(w.len + 1));
        }
        uint8_t fb[96];                               // focused-target RSSI for HUNT
        ss_cbor_w_t fw; ss_cbor_w_init(&fw, fb + 1, sizeof(fb) - 1);
        if (det_pop_focus_event(&fw)) {
            fb[0] = SS_EVT_RSSI_TICK;
            uart_slave_send(SS_T_EVENT, 0, 0, fb, (uint16_t)(fw.len + 1));
        }
    } else {
        // Not subscribed: drain so the queue never backs up
        uint8_t b[400]; ss_cbor_w_t w; ss_cbor_w_init(&w, b, sizeof(b));
        while (det_pop_event(&w)) { w.len = 0; w.err = false; }
    }
    uint16_t mv = battery_mv();
    bool low = mv && mv <= SS_BATT_LOW_MV;
    uint32_t evt_period = power_radio_allowed() ? 10000 : SS_CHARGE_STATUS_EVT_MS;
    if ((low && !S.low_reported) || (now - S.last_evt_ms) >= evt_period) {
        S.last_evt_ms = now;
        S.low_reported = low;
        send_status_event();
    }
}
