// SPDX-License-Identifier: GPL-3.0-or-later
// ss_frame.h — UART frame encoder and streaming parser (SHARED).
// Implements the framing in PROTOCOL.md section 2:
//   SOF VER TYPE SEQ FLAGS LEN(u16le) PAYLOAD CRC16(u16le)
// CRC16-CCITT is computed over VER..end-of-PAYLOAD. No heap, no OS calls.
#pragma once
#include "ss_protocol.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SS_FRAME_OVERHEAD 9u   // 7-byte header + 2-byte CRC

// Encode one frame into out[cap]. Returns total bytes written, or 0 if it does
// not fit / payload too large.
size_t ss_frame_encode(uint8_t* out, size_t cap, uint8_t type, uint8_t seq,
                       uint8_t flags, const uint8_t* payload, uint16_t len);

typedef enum {
    SS_PARSE_NONE  = 0,  // keep feeding bytes
    SS_PARSE_FRAME = 1,  // a valid frame is in `out`
    SS_PARSE_ERROR = 2,  // a frame was rejected; see out->err (and out->seq_valid)
} ss_parse_result_t;

typedef struct {
    uint8_t        type, seq, flags;
    uint16_t       len;
    const uint8_t* payload;   // points into the parser's buffer; valid until next feed
    ss_err_t       err;       // set when result is SS_PARSE_ERROR
    bool           seq_valid; // header was fully received, so seq/type are meaningful
} ss_frame_t;

typedef struct {
    uint8_t* buf;        // caller-provided, at least SS_MAX_PAYLOAD bytes
    uint8_t  state;
    uint8_t  hdr[6];     // VER TYPE SEQ FLAGS LEN_LO LEN_HI
    uint8_t  hdr_n;
    uint16_t len, got;
    uint16_t crc;
    uint8_t  crc_lo;
    uint8_t  crc_n;
} ss_parser_t;

void ss_parser_init(ss_parser_t* p, uint8_t* payload_buf);
// Feed one received byte. Resynchronises on SS_FRAME_SOF after any error.
ss_parse_result_t ss_parser_feed(ss_parser_t* p, uint8_t byte, ss_frame_t* out);

#ifdef __cplusplus
}
#endif
