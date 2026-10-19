// ss_token.h — SignalSweep-CYD consent token (SHARED).
// A token authorizes exactly one ACTIVE op, is minted by the CYD ONLY after a
// physical touch-confirm, and is verifiable ONLY by the C5 (bound to a session
// secret the laptop never sees). See PROTOCOL.md §5.
//
// Threat model is narrow and explicit: stop the laptop / a creative model from
// self-authorizing a detectable or transmitting op. It is NOT a defence against
// someone with a probe on your own UART header.
#pragma once
#include "ss_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SS_TOKEN_TTL_MS_DEFAULT  8000u   // short: authorize, then use promptly
#define SS_SESSION_SECRET_LEN    32u     // exchanged during SS_OP_HELLO
#define SS_NONCE_WINDOW          16u     // C5 remembers this many recent nonces

// Full token as held in RAM on the CYD before it is signed + serialized.
typedef struct {
    uint16_t cap_id;     // the capability (opcode) this authorizes — one only
    uint16_t nonce;      // single-use; C5 rejects replays within its window
    uint32_t issued_ms;  // CYD monotonic clock at mint time
    uint32_t ttl_ms;     // validity window
} ss_token_core_t;

// ---- Wire form --------------------------------------------------------------
// 8 bytes on the wire (SS_TOKEN_WIRE_LEN): a 6-byte compressed core + a 2-byte
// truncated MAC tag. The FULL core is fed to the MAC so truncation on the wire
// doesn't weaken binding of cap_id/nonce/time — the C5 reconstructs and re-MACs.
// Layout: [cap_id:16][nonce:16][issued_ms low 16][tag:16]  (ttl is a shared const)
// If you need per-op variable TTL, widen the wire form and update both sides here.

// ---- CYD side (mint) --------------------------------------------------------
// Called only from the consent flow, only after AUTHORIZE is tapped.
// `out8` must point to SS_TOKEN_WIRE_LEN bytes. Returns false if no session.
bool ss_token_mint(const ss_token_core_t* core,
                   const uint8_t session_secret[SS_SESSION_SECRET_LEN],
                   uint8_t out8[SS_TOKEN_WIRE_LEN]);

// ---- C5 side (verify) -------------------------------------------------------
// Verifies: MAC matches, not expired (now_ms vs issued+ttl), cap_id == opcode,
// and nonce not already seen (a successful verify consumes the nonce).
// Returns SS_OK or SS_ERR_TOKEN. This does NOT check the mode: the caller does
// that via ss_cap_check(). `now_ms` must be on the CYD's clock: the C5 adds the
// offset it measured during HELLO to its own millisecond counter.
ss_err_t ss_token_verify(const uint8_t in8[SS_TOKEN_WIRE_LEN],
                         ss_opcode_t  expected_op,
                         uint32_t     now_ms,
                         const uint8_t session_secret[SS_SESSION_SECRET_LEN]);

// MAC primitive: recommend HMAC-SHA256 (mbedTLS ships in the ESP32 core on both
// boards) truncated to the 2-byte wire tag. Keep the implementation identical on
// both sides — put it in a shared ss_token.c compiled into each firmware.

// C5: forget remembered nonces. Called on every HELLO (new session, new secret).
void ss_token_reset_replay_window(void);

#ifdef __cplusplus
}
#endif
