// SPDX-License-Identifier: GPL-3.0-or-later
#include "ss_frame.h"
#include <string.h>

size_t ss_frame_encode(uint8_t* out, size_t cap, uint8_t type, uint8_t seq,
                       uint8_t flags, const uint8_t* payload, uint16_t len) {
    if (len > SS_MAX_PAYLOAD) return 0;
    if (len && !payload) return 0;
    size_t total = SS_FRAME_OVERHEAD + len;
    if (cap < total) return 0;
    out[0] = SS_FRAME_SOF;
    out[1] = SS_PROTO_VERSION;
    out[2] = type;
    out[3] = seq;
    out[4] = flags;
    out[5] = (uint8_t)(len & 0xFF);
    out[6] = (uint8_t)(len >> 8);
    if (len) memcpy(out + 7, payload, len);
    uint16_t crc = ss_crc16(out + 1, (uint16_t)(6 + len));   // VER..end of payload
    out[7 + len] = (uint8_t)(crc & 0xFF);
    out[8 + len] = (uint8_t)(crc >> 8);
    return total;
}

enum { S_SOF = 0, S_HDR, S_PAYLOAD, S_CRC };

void ss_parser_init(ss_parser_t* p, uint8_t* payload_buf) {
    memset(p, 0, sizeof(*p));
    p->buf = payload_buf;
    p->state = S_SOF;
}

static void reset(ss_parser_t* p) { p->state = S_SOF; p->hdr_n = 0; p->got = 0; p->crc_n = 0; }

static ss_parse_result_t fail(ss_parser_t* p, ss_frame_t* out, ss_err_t err, bool seq_valid) {
    memset(out, 0, sizeof(*out));
    out->err = err;
    out->seq_valid = seq_valid;
    if (seq_valid) { out->type = p->hdr[1]; out->seq = p->hdr[2]; out->flags = p->hdr[3]; }
    reset(p);
    return SS_PARSE_ERROR;
}

ss_parse_result_t ss_parser_feed(ss_parser_t* p, uint8_t b, ss_frame_t* out) {
    switch (p->state) {
    case S_SOF:
        if (b == SS_FRAME_SOF) { p->state = S_HDR; p->hdr_n = 0; p->crc = SS_CRC16_INIT; }
        return SS_PARSE_NONE;

    case S_HDR:
        p->hdr[p->hdr_n++] = b;
        p->crc = ss_crc16_update(p->crc, b);
        if (p->hdr_n < 6) return SS_PARSE_NONE;
        p->len = (uint16_t)(p->hdr[4] | (p->hdr[5] << 8));
        if (p->hdr[0] != SS_PROTO_VERSION) return fail(p, out, SS_ERR_VERSION, true);
        if (p->len > SS_MAX_PAYLOAD)       return fail(p, out, SS_ERR_BADFRAME, true);
        p->got = 0;
        p->crc_n = 0;
        p->state = p->len ? S_PAYLOAD : S_CRC;
        return SS_PARSE_NONE;

    case S_PAYLOAD:
        p->buf[p->got++] = b;
        p->crc = ss_crc16_update(p->crc, b);
        if (p->got >= p->len) p->state = S_CRC;
        return SS_PARSE_NONE;

    case S_CRC:
        if (p->crc_n == 0) { p->crc_lo = b; p->crc_n = 1; return SS_PARSE_NONE; }
        {
            uint16_t rx = (uint16_t)(p->crc_lo | (b << 8));
            if (rx != p->crc) return fail(p, out, SS_ERR_CRC, true);
            out->type = p->hdr[1];
            out->seq = p->hdr[2];
            out->flags = p->hdr[3];
            out->len = p->len;
            out->payload = p->buf;
            out->err = SS_OK;
            out->seq_valid = true;
            reset(p);
            return SS_PARSE_FRAME;
        }
    }
    reset(p);
    return SS_PARSE_NONE;
}
