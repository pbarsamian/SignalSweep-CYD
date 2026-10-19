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
#include "ss_protocol.h"
#include "ss_capabilities.h"
#include "ss_cbor.h"
#include "ss_frame.h"
#include "ss_token.h"
#include "mode.h"
#include "uart_link.h"
#include "selftest.h"
#include "status_ui.h"

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
}

static void handle_status_resp(const uint8_t* cb, size_t n) {
    uint32_t u = 0; bool b = false;
    c5.have = true;
    c5.mode = ss_cbor_get_uint(cb, n, "mode", &u) ? (uint8_t)u : 0;
    c5.shadow_valid = ss_cbor_get_bool(cb, n, "valid", &b) && b;
    c5.up_ms = ss_cbor_get_uint(cb, n, "up_ms", &u) ? u : 0;
    c5.batt_mv = ss_cbor_get_uint(cb, n, "batt_mv", &u) ? (uint16_t)u : 0;
    c5.batt_low = ss_cbor_get_bool(cb, n, "batt_low", &b) && b;
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

static void on_event(uint8_t kind, const uint8_t*, size_t n) {
    Serial.printf("[event] kind=%u (%u B)\n", kind, (unsigned)n);
}

static void on_timeout(uint8_t op) {
    if (op == SS_OP_HELLO) { g_hello_inflight = false; g_next_hello_ms = millis() + 1000; return; }
    if (op == SS_OP_GET_STATUS) {
        if (++g_hb_misses >= 3) link_down("heartbeat lost");
        return;
    }
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
        case 'w': send_simple(SS_OP_SCAN_WIFI); break;
        case 'b': send_simple(SS_OP_SCAN_BLE); break;
        case 'l': send_simple(SS_OP_LIST_DETECTIONS); break;
        case 'g': send_simple(SS_OP_GATT_CONNECT_READ); break;     // gated: expect ERR_MODE
        case 'a': send_simple(SS_OP_TX_SLOT_A); break;             // gated: expect ERR_MODE
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
                           "a TX_SLOT_A(no token)  u unknown-op  x bad-CRC  v bad-version");
            break;
        default: break;
        }
    }
}

// ---- Arduino ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[SignalSweep CYD split M2] boot, mode RX-ONLY");

    status_ui_init();
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
        g_next_hb_ms = now + SS_HEARTBEAT_MS;
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
    static uint32_t drawn = 0;
    if (now - drawn >= 250) { drawn = now; status_ui_draw(view, mode_get()); }
    delay(1);
}
