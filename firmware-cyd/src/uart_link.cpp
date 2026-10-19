// SPDX-License-Identifier: GPL-3.0-or-later
#include "uart_link.h"
#include <string.h>

// Staging buffer: opcode + token + CBOR (<= SS_MAX_PAYLOAD) and the framed copy.
#define TX_STAGE (SS_MAX_PAYLOAD + 16)
#define TX_FRAME (SS_MAX_PAYLOAD + SS_FRAME_OVERHEAD + 16)

void UartLink::begin(HardwareSerial& port, int rx_pin, int tx_pin, uint32_t baud) {
    s_ = &port;
    rxbuf_ = (uint8_t*)malloc(SS_MAX_PAYLOAD);
    txbuf_ = (uint8_t*)malloc(TX_STAGE + TX_FRAME);
    ss_parser_init(&parser_, rxbuf_);
    port.setRxBufferSize(8192);                       // must precede begin()
    port.begin(baud, SERIAL_8N1, rx_pin, tx_pin);
    Serial.printf("[link] Serial2 RX=%d TX=%d @%u\n", rx_pin, tx_pin, (unsigned)baud);
}

uint8_t UartLink::next_seq() {
    seq_++;
    if (seq_ == 0) seq_ = 1;          // SEQ 0 is reserved for EVENTs
    return seq_;
}

bool UartLink::busy() const {
    for (int i = 0; i < MAXP; i++) if (pend_[i].used) return true;
    return false;
}

size_t UartLink::build_req_frame(uint8_t* out, size_t cap, uint8_t op, const uint8_t* cbor, size_t n) {
    if (n + 1 > SS_MAX_PAYLOAD) return 0;
    uint8_t* stage = txbuf_;
    stage[0] = op;
    if (n) memcpy(stage + 1, cbor, n);
    return ss_frame_encode(out, cap, SS_T_REQ, next_seq(), 0, stage, (uint16_t)(n + 1));
}

bool UartLink::send_raw(const uint8_t* bytes, size_t n) {
    if (!s_) return false;
    s_->write(bytes, n);
    return true;
}

int UartLink::send_req(uint8_t op, const uint8_t* token8, const uint8_t* cbor, size_t n) {
    if (!s_) return -1;
    size_t tlen = token8 ? SS_TOKEN_WIRE_LEN : 0;
    if (1 + tlen + n > SS_MAX_PAYLOAD) return -1;
    int slot = -1;
    for (int i = 0; i < MAXP; i++) if (!pend_[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    uint8_t* stage = txbuf_;
    uint8_t* frame = txbuf_ + TX_STAGE;
    size_t p = 0;
    stage[p++] = op;
    if (token8) { memcpy(stage + p, token8, SS_TOKEN_WIRE_LEN); p += SS_TOKEN_WIRE_LEN; }
    if (n) { memcpy(stage + p, cbor, n); p += n; }

    uint8_t seq = next_seq();
    uint8_t flags = token8 ? SS_FLAG_HAS_TOKEN : 0;
    size_t fl = ss_frame_encode(frame, TX_FRAME, SS_T_REQ, seq, flags, stage, (uint16_t)p);
    if (!fl) return -1;
    s_->write(frame, fl);
    pend_[slot] = { true, seq, op, millis() };
    stats.tx++;
    return seq;
}

void UartLink::handle_frame(const ss_frame_t& f) {
    stats.rx++;
    switch (f.type) {
    case SS_T_RESP: {
        if (f.len < 1) { stats.crc_err++; return; }
        int slot = -1;
        for (int i = 0; i < MAXP; i++) if (pend_[i].used && pend_[i].seq == f.seq) { slot = i; break; }
        if (slot < 0) { stats.stray++; return; }
        uint8_t op = pend_[slot].op;
        uint32_t rtt = millis() - pend_[slot].sent_ms;
        pend_[slot].used = false;
        stats.last_rtt_ms = rtt;
        uint8_t status = f.payload[0];
        if (status == SS_OK) stats.resp_ok++; else stats.resp_err++;
        if (on_resp) on_resp(op, status, f.payload + 1, f.len - 1, rtt);
        break;
    }
    case SS_T_EVENT:
        if (f.len >= 1 && on_event) on_event(f.payload[0], f.payload + 1, f.len - 1);
        break;
    case SS_T_NACK: {
        stats.nack++;
        uint8_t err = f.len >= 1 ? f.payload[0] : (uint8_t)SS_ERR_BADFRAME;
        uint8_t op = 0;
        for (int i = 0; i < MAXP; i++)
            if (pend_[i].used && pend_[i].seq == f.seq) { op = pend_[i].op; pend_[i].used = false; break; }
        if (on_nack) on_nack(err, op);
        break;
    }
    default: break;                   // ACK is unused for now
    }
}

void UartLink::poll() {
    if (!s_) return;
    ss_frame_t f;
    while (s_->available()) {
        ss_parse_result_t r = ss_parser_feed(&parser_, (uint8_t)s_->read(), &f);
        if (r == SS_PARSE_FRAME) handle_frame(f);
        else if (r == SS_PARSE_ERROR) stats.crc_err++;
    }
    uint32_t now = millis();
    for (int i = 0; i < MAXP; i++) {
        if (pend_[i].used && now - pend_[i].sent_ms > SS_REQ_TIMEOUT_MS) {
            uint8_t op = pend_[i].op;
            pend_[i].used = false;
            stats.timeouts++;
            if (on_timeout) on_timeout(op);
        }
    }
}
