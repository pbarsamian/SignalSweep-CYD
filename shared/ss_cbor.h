// SPDX-License-Identifier: GPL-3.0-or-later
// ss_cbor.h — minimal CBOR (RFC 8949) encoder/decoder for payload args/results
// (SHARED). Definite-length items only; maps use text keys. No heap.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- encoder ----------------------------------------------------------------
// Writes never overrun: on overflow `err` latches and later writes are no-ops.
typedef struct { uint8_t* buf; size_t cap, len; bool err; } ss_cbor_w_t;

void ss_cbor_w_init(ss_cbor_w_t* w, uint8_t* buf, size_t cap);
void ss_cbor_map(ss_cbor_w_t* w, size_t pairs);      // then 2*pairs items (key, value)
void ss_cbor_array(ss_cbor_w_t* w, size_t items);
void ss_cbor_uint(ss_cbor_w_t* w, uint64_t v);
void ss_cbor_int(ss_cbor_w_t* w, int64_t v);
void ss_cbor_bool(ss_cbor_w_t* w, bool v);
void ss_cbor_text(ss_cbor_w_t* w, const char* s);
void ss_cbor_text_n(ss_cbor_w_t* w, const char* s, size_t n);
void ss_cbor_bytes(ss_cbor_w_t* w, const uint8_t* p, size_t n);
// key + value convenience
void ss_cbor_kv_uint(ss_cbor_w_t* w, const char* key, uint64_t v);
void ss_cbor_kv_int(ss_cbor_w_t* w, const char* key, int64_t v);
void ss_cbor_kv_bool(ss_cbor_w_t* w, const char* key, bool v);
void ss_cbor_kv_text(ss_cbor_w_t* w, const char* key, const char* s);
void ss_cbor_kv_bytes(ss_cbor_w_t* w, const char* key, const uint8_t* p, size_t n);

// ---- decoder ----------------------------------------------------------------
typedef enum {
    SS_CBOR_NONE = 0, SS_CBOR_UINT, SS_CBOR_NEGINT, SS_CBOR_BYTES, SS_CBOR_TEXT,
    SS_CBOR_BOOL, SS_CBOR_NULL, SS_CBOR_ARRAY, SS_CBOR_MAP, SS_CBOR_OTHER
} ss_cbor_type_t;

typedef struct {
    ss_cbor_type_t type;
    uint64_t       u;      // UINT value, BOOL (0/1), or NEGINT magnitude-1
    int64_t        i;      // UINT/NEGINT as signed (saturated)
    const uint8_t* ptr;    // BYTES / TEXT data (TEXT is NOT NUL-terminated); for ARRAY / MAP,
                           // the first contained item (walk with ss_cbor_skip)
    size_t         n;      // BYTES / TEXT length, ARRAY / MAP item count
} ss_cbor_val_t;

// Find `key` in the top-level map of buf[0..len). True if found and the whole
// map is well-formed up to that point.
bool ss_cbor_map_get(const uint8_t* buf, size_t len, const char* key, ss_cbor_val_t* out);
// Advance *pp past one whole item (nested maps/arrays included). False if malformed.
// To walk an ARRAY value v: p = v.ptr; repeat v.n times { use item at p; ss_cbor_skip(&p, end); }
// An item that is a map can be queried in place with ss_cbor_map_get(p, end - p, key, &out).
bool ss_cbor_skip(const uint8_t** pp, const uint8_t* end);
// Typed helpers (false if the key is missing or has the wrong type / range)
bool ss_cbor_get_uint(const uint8_t* buf, size_t len, const char* key, uint32_t* out);
bool ss_cbor_get_bool(const uint8_t* buf, size_t len, const char* key, bool* out);
bool ss_cbor_get_bytes(const uint8_t* buf, size_t len, const char* key,
                       uint8_t* out, size_t want_len);   // exact length required

#ifdef __cplusplus
}
#endif
