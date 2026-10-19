// SPDX-License-Identifier: GPL-3.0-or-later
// uart_link.h — CYD side of the C5 link: frames REQs, matches RESPs by SEQ,
// surfaces EVENTs and NACKs. Non-blocking: call poll() every loop().
#pragma once
#include <Arduino.h>
#include "ss_protocol.h"
#include "ss_frame.h"

struct LinkStats {
    uint32_t tx = 0, rx = 0;          // frames sent / valid frames received
    uint32_t resp_ok = 0, resp_err = 0;
    uint32_t nack = 0;                // NACK frames received
    uint32_t crc_err = 0;             // bad frames seen by OUR parser
    uint32_t timeouts = 0;
    uint32_t stray = 0;               // RESP with no matching REQ
    uint32_t last_rtt_ms = 0;
    uint32_t raw_bytes = 0;           // every byte seen on the RX pin, valid or not
};

// Callbacks run from poll(). `cbor` points into link-owned buffers: copy it out
// if you need it after the callback returns.
typedef void (*LinkRespCb)(uint8_t op, uint8_t status, const uint8_t* cbor, size_t n, uint32_t rtt_ms);
typedef void (*LinkNackCb)(uint8_t err, uint8_t op /*0 if unknown*/);
typedef void (*LinkEventCb)(uint8_t kind, const uint8_t* cbor, size_t n);
typedef void (*LinkTimeoutCb)(uint8_t op);

class UartLink {
public:
    LinkStats stats;
    LinkRespCb    on_resp = nullptr;
    LinkNackCb    on_nack = nullptr;
    LinkEventCb   on_event = nullptr;
    LinkTimeoutCb on_timeout = nullptr;

    void begin(HardwareSerial& port, int rx_pin, int tx_pin, uint32_t baud);
    void poll();

    // REQ payload = [op][token8 if given][cbor args]. Returns the SEQ used, or -1
    // if the pending table is full or the payload is too large.
    int send_req(uint8_t op, const uint8_t* token8, const uint8_t* cbor, size_t n);

    // Bring-up/test hooks: put arbitrary bytes on the wire (bad CRC, bad version...)
    bool send_raw(const uint8_t* bytes, size_t n);
    // Build a well-formed REQ frame into `out` without sending (for corrupting)
    size_t build_req_frame(uint8_t* out, size_t cap, uint8_t op, const uint8_t* cbor, size_t n);

    bool busy() const;                // any REQ still awaiting its RESP

private:
    struct Pending { bool used; uint8_t seq, op; uint32_t sent_ms; };
    static const int MAXP = 4;
    HardwareSerial* s_ = nullptr;
    Pending pend_[MAXP] = {};
    uint8_t seq_ = 0;
    ss_parser_t parser_;
    uint8_t* rxbuf_ = nullptr;        // payload buffer for the parser (SS_MAX_PAYLOAD)
    uint8_t* txbuf_ = nullptr;        // frame + payload staging
    uint8_t  next_seq();
    void handle_frame(const ss_frame_t& f);
};
