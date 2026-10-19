// SPDX-License-Identifier: GPL-3.0-or-later
#include "selftest.h"
#include <Arduino.h>
#include <string.h>
#include "ss_protocol.h"
#include "ss_capabilities.h"
#include "ss_frame.h"
#include "ss_cbor.h"
#include "ss_token.h"

static int g_pass, g_total;

static void check(const char* name, bool ok) {
    g_total++;
    if (ok) g_pass++;
    Serial.printf("[selftest] %-28s %s\n", name, ok ? "PASS" : "FAIL");
}

static bool test_crc() {
    // CRC-16/CCITT-FALSE standard check value
    return ss_crc16((const uint8_t*)"123456789", 9) == 0x29B1;
}

static bool test_frame_roundtrip() {
    static uint8_t payload[64], pbuf[SS_MAX_PAYLOAD], frame[128];
    for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i * 7 + 3);
    size_t n = ss_frame_encode(frame, sizeof(frame), SS_T_REQ, 42, SS_FLAG_HAS_TOKEN, payload, 64);
    if (n != SS_FRAME_OVERHEAD + 64) return false;
    ss_parser_t p; ss_parser_init(&p, pbuf);
    ss_frame_t f; ss_parse_result_t r = SS_PARSE_NONE;
    for (size_t i = 0; i < n; i++) {
        r = ss_parser_feed(&p, frame[i], &f);
        if (i + 1 < n && r != SS_PARSE_NONE) return false;
    }
    return r == SS_PARSE_FRAME && f.type == SS_T_REQ && f.seq == 42 &&
           f.flags == SS_FLAG_HAS_TOKEN && f.len == 64 && memcmp(f.payload, payload, 64) == 0;
}

static bool test_frame_bad_crc() {
    static uint8_t payload[8] = {1,2,3,4,5,6,7,8}, pbuf[SS_MAX_PAYLOAD], frame[64];
    size_t n = ss_frame_encode(frame, sizeof(frame), SS_T_REQ, 9, 0, payload, 8);
    frame[9] ^= 0x40;                              // corrupt one payload byte
    ss_parser_t p; ss_parser_init(&p, pbuf);
    ss_frame_t f; ss_parse_result_t r = SS_PARSE_NONE;
    for (size_t i = 0; i < n; i++) r = ss_parser_feed(&p, frame[i], &f);
    return r == SS_PARSE_ERROR && f.err == SS_ERR_CRC && f.seq_valid && f.seq == 9;
}

static bool test_frame_bad_version() {
    static uint8_t payload[4] = {1,2,3,4}, pbuf[SS_MAX_PAYLOAD], frame[64];
    size_t n = ss_frame_encode(frame, sizeof(frame), SS_T_REQ, 5, 0, payload, 4);
    frame[1] = 0x7F;                               // wrong protocol version
    ss_parser_t p; ss_parser_init(&p, pbuf);
    ss_frame_t f; bool got = false;
    for (size_t i = 0; i < n && !got; i++)
        got = ss_parser_feed(&p, frame[i], &f) == SS_PARSE_ERROR;
    return got && f.err == SS_ERR_VERSION;
}

static bool test_frame_resync() {
    static uint8_t payload[3] = {9,8,7}, pbuf[SS_MAX_PAYLOAD], frame[64];
    size_t n = ss_frame_encode(frame, sizeof(frame), SS_T_RESP, 77, 0, payload, 3);
    ss_parser_t p; ss_parser_init(&p, pbuf);
    ss_frame_t f; bool got = false;
    const uint8_t junk[] = {0x00, 0xFF, 0x13, 0x37};   // no SOF in here
    for (uint8_t b : junk) ss_parser_feed(&p, b, &f);
    for (size_t i = 0; i < n; i++)
        if (ss_parser_feed(&p, frame[i], &f) == SS_PARSE_FRAME) got = true;
    return got && f.seq == 77 && f.len == 3;
}

static bool test_cbor() {
    uint8_t buf[128];
    uint8_t key32[32]; for (int i = 0; i < 32; i++) key32[i] = (uint8_t)i;
    ss_cbor_w_t w; ss_cbor_w_init(&w, buf, sizeof(buf));
    ss_cbor_map(&w, 5);
    ss_cbor_kv_uint(&w, "big", 0x12345678u);
    ss_cbor_kv_bool(&w, "ok", true);
    ss_cbor_kv_text(&w, "name", "c5");
    ss_cbor_kv_bytes(&w, "key", key32, 32);
    ss_cbor_kv_int(&w, "neg", -5);
    if (w.err) return false;
    uint32_t u = 0; bool b = false; uint8_t k[32]; ss_cbor_val_t v;
    if (!ss_cbor_get_uint(buf, w.len, "big", &u) || u != 0x12345678u) return false;
    if (!ss_cbor_get_bool(buf, w.len, "ok", &b) || !b) return false;
    if (!ss_cbor_get_bytes(buf, w.len, "key", k, 32) || memcmp(k, key32, 32) != 0) return false;
    if (!ss_cbor_map_get(buf, w.len, "name", &v) || v.type != SS_CBOR_TEXT || v.n != 2 ||
        memcmp(v.ptr, "c5", 2) != 0) return false;
    if (!ss_cbor_map_get(buf, w.len, "neg", &v) || v.type != SS_CBOR_NEGINT || v.i != -5) return false;
    if (ss_cbor_get_uint(buf, w.len, "missing", &u)) return false;
    if (ss_cbor_get_uint(buf, w.len, "ok", &u)) return false;      // wrong type
    // overflow latches instead of overrunning
    uint8_t tiny[4]; ss_cbor_w_t t; ss_cbor_w_init(&t, tiny, sizeof(tiny));
    ss_cbor_text(&t, "this does not fit");
    return t.err;
}

static bool test_cap_table() {
    // Every registry row is findable; unknown opcodes are rejected.
    for (size_t i = 0; i < SS_CAP_COUNT; i++)
        if (ss_cap_find(SS_CAP_TABLE[i].opcode) != &SS_CAP_TABLE[i]) return false;
    return ss_cap_find((ss_opcode_t)0x7E) == NULL &&
           ss_cap_check((ss_opcode_t)0x7E, SS_MODE_SILENT_RX, true) == SS_ERR_OPCODE;
}

static bool test_cap_gate() {
    // Truth table: class x mode x token-valid. Only PASSIVE runs in SILENT_RX;
    // ACTIVE_* need TX mode AND a valid token; a TX-mode ACTIVE op without a token
    // is a token error, never a mode error.
    for (size_t i = 0; i < SS_CAP_COUNT; i++) {
        const ss_cap_t& c = SS_CAP_TABLE[i];
        for (int m = 0; m < 2; m++) for (int t = 0; t < 2; t++) {
            ss_err_t got = ss_cap_check(c.opcode, (ss_mode_t)m, t);
            ss_err_t want;
            if (c.cls == SS_CLS_PASSIVE) want = SS_OK;
            else if (m == SS_MODE_SILENT_RX) want = SS_ERR_MODE;
            else want = t ? SS_OK : SS_ERR_TOKEN;
            if (got != want) return false;
        }
    }
    return true;
}

static bool test_token() {
    uint8_t secret[SS_SESSION_SECRET_LEN], other[SS_SESSION_SECRET_LEN];
    for (int i = 0; i < SS_SESSION_SECRET_LEN; i++) { secret[i] = (uint8_t)(0xA0 + i); other[i] = (uint8_t)i; }
    ss_token_reset_replay_window();

    ss_token_core_t c = { SS_OP_GATT_CONNECT_READ, 1001, 100000u, SS_TOKEN_TTL_MS_DEFAULT };
    uint8_t tok[SS_TOKEN_WIRE_LEN];
    if (!ss_token_mint(&c, secret, tok)) return false;
    if (ss_token_verify(tok, SS_OP_GATT_CONNECT_READ, 100500u, secret) != SS_OK) return false;  // happy path
    if (ss_token_verify(tok, SS_OP_GATT_CONNECT_READ, 100600u, secret) == SS_OK) return false;  // replay

    c.nonce = 1002; ss_token_mint(&c, secret, tok);
    if (ss_token_verify(tok, SS_OP_DEVICE_INFO_READ, 100500u, secret) == SS_OK) return false;   // wrong op

    c.nonce = 1003; ss_token_mint(&c, secret, tok);
    if (ss_token_verify(tok, SS_OP_GATT_CONNECT_READ, 100500u, other) == SS_OK) return false;   // wrong secret

    c.nonce = 1004; ss_token_mint(&c, secret, tok);
    uint8_t bad[SS_TOKEN_WIRE_LEN]; memcpy(bad, tok, sizeof(bad)); bad[6] ^= 1;
    if (ss_token_verify(bad, SS_OP_GATT_CONNECT_READ, 100500u, secret) == SS_OK) return false;  // tampered tag

    c.nonce = 1005; ss_token_mint(&c, secret, tok);
    if (ss_token_verify(tok, SS_OP_GATT_CONNECT_READ, 100000u + SS_TOKEN_TTL_MS_DEFAULT + 2000u,
                        secret) == SS_OK) return false;                                          // expired

    c.nonce = 1006; ss_token_mint(&c, secret, tok);
    if (ss_token_verify(tok, SS_OP_GATT_CONNECT_READ, 100100u, secret) != SS_OK) return false;  // still fresh

    c.ttl_ms = 999;                                                                              // non-default ttl refused
    if (ss_token_mint(&c, secret, tok)) return false;

    ss_token_reset_replay_window();
    return true;
}

SelfTestResult selftest_run() {
    g_pass = g_total = 0;
    check("crc16 vector (29B1)",      test_crc());
    check("frame round trip",         test_frame_roundtrip());
    check("frame bad CRC -> ERR_CRC", test_frame_bad_crc());
    check("frame bad version",        test_frame_bad_version());
    check("frame resync after junk",  test_frame_resync());
    check("cbor round trip",          test_cbor());
    check("capability table",         test_cap_table());
    check("capability gate truth",    test_cap_gate());
    check("token mint/verify/replay", test_token());
    Serial.printf("[selftest] %d/%d passed\n", g_pass, g_total);
    return { g_pass, g_total };
}
