// SPDX-License-Identifier: GPL-3.0-or-later
// ss_token.c — consent-token mint (CYD) / verify (C5). SHARED, compiled into both.
// HMAC-SHA256 (mbedTLS, present in both the Arduino-ESP32 core and ESP-IDF),
// truncated to the 2-byte wire tag. See ss_token.h and PROTOCOL.md section 5.
#include "ss_token.h"
#include <string.h>
#include "mbedtls/md.h"

// Tolerated clock skew, in ms, between the CYD's issue time and the C5's
// offset-adjusted clock (UART latency + the HELLO clock-offset estimate).
#define SS_TOKEN_SKEW_MS 750u

static void put_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Tag = first 2 bytes of HMAC-SHA256(secret, FULL core serialized little-endian).
static bool compute_tag(uint16_t cap_id, uint16_t nonce, uint32_t issued_ms, uint32_t ttl_ms,
                        const uint8_t secret[SS_SESSION_SECRET_LEN], uint8_t tag[2]) {
    uint8_t msg[12];
    put_le16(msg + 0, cap_id);
    put_le16(msg + 2, nonce);
    put_le32(msg + 4, issued_ms);
    put_le32(msg + 8, ttl_ms);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    uint8_t mac[32];
    if (mbedtls_md_hmac(info, secret, SS_SESSION_SECRET_LEN, msg, sizeof(msg), mac) != 0) return false;
    tag[0] = mac[0]; tag[1] = mac[1];
    memset(mac, 0, sizeof(mac));
    return true;
}

bool ss_token_mint(const ss_token_core_t* core,
                   const uint8_t secret[SS_SESSION_SECRET_LEN],
                   uint8_t out8[SS_TOKEN_WIRE_LEN]) {
    if (!core || !secret || !out8) return false;
    if (core->ttl_ms != SS_TOKEN_TTL_MS_DEFAULT) return false;   // ttl is a shared constant
    uint8_t tag[2];
    if (!compute_tag(core->cap_id, core->nonce, core->issued_ms, core->ttl_ms, secret, tag)) return false;
    put_le16(out8 + 0, core->cap_id);
    put_le16(out8 + 2, core->nonce);
    put_le16(out8 + 4, (uint16_t)(core->issued_ms & 0xFFFF));
    out8[6] = tag[0];
    out8[7] = tag[1];
    return true;
}

// Recent-nonce window (C5 side; unused on the CYD). Single-threaded use only:
// the C5 dispatcher is the sole caller.
static uint16_t s_nonce[SS_NONCE_WINDOW];
static uint8_t  s_nonce_n = 0;     // valid entries
static uint8_t  s_nonce_head = 0;  // next slot to overwrite

static bool nonce_seen(uint16_t n) {
    for (uint8_t i = 0; i < s_nonce_n; i++) if (s_nonce[i] == n) return true;
    return false;
}
static void nonce_remember(uint16_t n) {
    s_nonce[s_nonce_head] = n;
    s_nonce_head = (uint8_t)((s_nonce_head + 1) % SS_NONCE_WINDOW);
    if (s_nonce_n < SS_NONCE_WINDOW) s_nonce_n++;
}

void ss_token_reset_replay_window(void) { s_nonce_n = 0; s_nonce_head = 0; }

ss_err_t ss_token_verify(const uint8_t in8[SS_TOKEN_WIRE_LEN],
                         ss_opcode_t  expected_op,
                         uint32_t     now_ms,
                         const uint8_t secret[SS_SESSION_SECRET_LEN]) {
    if (!in8 || !secret) return SS_ERR_TOKEN;
    uint16_t cap_id = get_le16(in8 + 0);
    uint16_t nonce  = get_le16(in8 + 2);
    uint16_t low    = get_le16(in8 + 4);

    // Rebuild the full 32-bit issue time from the low 16 bits and the C5's idea of
    // "now". The candidate nearest to now (not more than SKEW in the future) wins.
    uint32_t issued = (now_ms & 0xFFFF0000u) | low;
    if (issued > now_ms + SS_TOKEN_SKEW_MS) issued -= 0x10000u;

    uint8_t tag[2];
    if (!compute_tag(cap_id, nonce, issued, SS_TOKEN_TTL_MS_DEFAULT, secret, tag)) return SS_ERR_TOKEN;
    // constant-time compare of the truncated tag
    uint8_t diff = (uint8_t)((tag[0] ^ in8[6]) | (tag[1] ^ in8[7]));
    if (diff != 0) return SS_ERR_TOKEN;

    if (cap_id != (uint16_t)expected_op) return SS_ERR_TOKEN;

    // Freshness: issued <= now + skew, and now <= issued + ttl (+ skew)
    if (issued > now_ms + SS_TOKEN_SKEW_MS) return SS_ERR_TOKEN;
    uint32_t age = (now_ms >= issued) ? (now_ms - issued) : 0;
    if (age > SS_TOKEN_TTL_MS_DEFAULT + SS_TOKEN_SKEW_MS) return SS_ERR_TOKEN;

    // Single-use: a valid MAC with a remembered nonce is a replay.
    if (nonce_seen(nonce)) return SS_ERR_TOKEN;
    nonce_remember(nonce);
    return SS_OK;
}
