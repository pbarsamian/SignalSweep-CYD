// ss_protocol.h — SignalSweep-CYD split-architecture wire contract.
// SHARED between CYD firmware and XIAO C5 firmware. Single source of truth for
// the UART link. Do not fork this file per board — both #include the same copy.
//
// See PROTOCOL.md for the prose spec. Pure definitions only; no logic here so it
// stays safe to include from both an Arduino .ino and an ESP-IDF build.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---- protocol version -------------------------------------------------------
#define SS_PROTO_VERSION   0x01u
#define SS_FRAME_SOF       0xA5u
#define SS_MAX_PAYLOAD     4096u
#define SS_TOKEN_WIRE_LEN  8u     // bytes on the wire when FLAGS.HAS_TOKEN set

// ---- link timing / rate ------------------------------------------------------
#define SS_UART_BAUD_BRINGUP  115200u   // M2 default; 921600 once framing is clean
#define SS_UART_BAUD_FAST     921600u
#define SS_HEARTBEAT_MS       2000u     // CYD sends GET_STATUS this often once linked
#define SS_SHADOW_STALE_MS    10000u    // C5: no frame from the CYD for this long =>
                                        //     mode shadow is stale => ACTIVE ops refused
#define SS_REQ_TIMEOUT_MS     600u      // CYD: give up on an unanswered REQ

// ---- charge-mode power savings (see PROTOCOL.md 3c) ------------------------------
// In charge mode the radio is off, so the link only needs to prove liveness slowly.
#define SS_CHARGE_HEARTBEAT_MS     15000u  // CYD GET_STATUS period (3 misses = link down after 45 s)
#define SS_CHARGE_SHADOW_STALE_MS  45000u  // C5 stale window while charging (> 3 heartbeats)
#define SS_CHARGE_STATUS_EVT_MS    30000u  // C5 STATUS event period while charging (10 s normally)
#define SS_CHARGE_WAKE_MS           8000u  // CYD screen stays on this long after a touch
// Optional hardware: the C5 can cut the CYD's supply through the boost converter's EN pin
// after this long in charge mode (needs a wire + pull-up, see BUILD_MANIFEST.md).
#define SS_CYD_CUT_DELAY_MS        30000u

// ---- frame TYPE -------------------------------------------------------------
typedef enum {
    SS_T_REQ   = 1,   // CYD -> C5 : invoke an opcode
    SS_T_RESP  = 2,   // C5 -> CYD : result for a REQ (matched by seq)
    SS_T_EVENT = 3,   // C5 -> CYD : unsolicited (new detection, rssi, tx-started)
    SS_T_ACK   = 4,
    SS_T_NACK  = 5,   // payload is [SS_ERR u8]
} ss_type_t;

// ---- FLAGS bitfield ---------------------------------------------------------
#define SS_FLAG_HAS_TOKEN   (1u << 0)  // payload begins with 8-byte token
#define SS_FLAG_MORE        (1u << 1)  // multi-frame response continues
#define SS_FLAG_COMPRESSED  (1u << 2)  // reserved

// ---- opcodes (the C5's job list) --------------------------------------------
// PASSIVE: no token, callable in any mode.
// ACTIVE_*: token required, MODE_TX_DETECTABLE only.
typedef enum {
    // control (no class / handled specially)
    SS_OP_HELLO             = 0x01, // handshake: versions + session secret
    SS_OP_SET_MODE          = 0x02, // CYD pushes authoritative mode to C5 shadow
    SS_OP_GET_STATUS        = 0x03,
    SS_OP_SET_POWER         = 0x04, // charge mode on/off (refused while forced: SS_ERR_LOCKED)
    SS_OP_SET_CONFIG        = 0x05, // detection-type toggles {drone,tracker,camera,bodycam,attack: bool}

    // PASSIVE — receive-only
    SS_OP_SCAN_WIFI         = 0x10, // listen-only; NO probe requests
    SS_OP_SCAN_BLE          = 0x11, // passive scan; NO scan-requests
    SS_OP_LIST_DETECTIONS   = 0x12,
    SS_OP_GET_DEVICE_PROFILE= 0x13,
    SS_OP_REMOTE_ID_DECODE  = 0x14, // ASTM F3411 BLE / beacon / NAN
    SS_OP_FOCUS_TARGET      = 0x15,
    SS_OP_SUBSCRIBE_EVENTS  = 0x16,

    // ACTIVE_CONNECT — initiates a connection => detectable
    SS_OP_GATT_CONNECT_READ = 0x30,
    SS_OP_DEVICE_INFO_READ  = 0x31,

    // ACTIVE_TRANSMIT — registry slots; handlers USER-SUPPLIED (see PROTOCOL.md §7)
    SS_OP_TX_SLOT_A         = 0x50,
    SS_OP_TX_SLOT_B         = 0x51,
    SS_OP_TX_SLOT_C         = 0x52,
    SS_OP_TX_SLOT_D         = 0x53,
} ss_opcode_t;

// ---- EVENT kinds (first payload byte of a TYPE=EVENT frame; see PROTOCOL.md 3a) ----
typedef enum {
    SS_EVT_DETECTION  = 1,
    SS_EVT_RSSI_TICK  = 2,
    SS_EVT_TX_STARTED = 3,
    SS_EVT_STATUS     = 4,  // periodic + on low battery: batt_mv, batt_low, mode_shadow
} ss_event_kind_t;

// Low-battery threshold for the STATUS event / GET_STATUS batt_low (millivolts). Tune.
#ifndef SS_BATT_LOW_MV
#define SS_BATT_LOW_MV  3400u   // test builds may override with -DSS_BATT_LOW_MV=...
#endif

// ---- error codes (NACK payload / RESP status) -------------------------------
typedef enum {
    SS_OK           = 0x00,
    SS_ERR_VERSION  = 0x01, // proto version mismatch
    SS_ERR_CRC      = 0x02,
    SS_ERR_BADFRAME = 0x03,
    SS_ERR_OPCODE   = 0x04, // unknown opcode
    SS_ERR_MODE     = 0x05, // not callable in current mode
    SS_ERR_TOKEN    = 0x06, // missing/invalid/expired token or nonce replay
    SS_ERR_BUSY     = 0x07, // radio busy / queue full
    SS_ERR_RADIO    = 0x08, // underlying RF failure
    SS_ERR_UNIMPL   = 0x09, // registry slot with no handler wired in
    SS_ERR_LOCKED   = 0x0A, // forced charge mode: battery must rise above SS_BATT_EXIT_MV first
} ss_err_t;

// ---- power state (CHARGE MODE) ---------------------------------------------
// Orthogonal to the RF mode below. In charge mode the C5 radio is off and the CYD
// shows only the battery voltage. The C5 owns this state because it measures the
// battery: it forces CHARGE at or below SS_BATT_LOW_MV and will not leave FORCED
// until the voltage is back above SS_BATT_EXIT_MV. The CYD mirrors it.
typedef enum {
    SS_POWER_NORMAL        = 0,
    SS_POWER_CHARGE_USER   = 1, // selected by the user; they can leave any time
    SS_POWER_CHARGE_FORCED = 2, // battery low; locked until above SS_BATT_EXIT_MV
} ss_power_t;
#ifndef SS_BATT_EXIT_MV
#define SS_BATT_EXIT_MV  4000u  // FORCED charge mode is held until the battery exceeds this
#endif

// ---- operating mode (authoritative on CYD, shadowed on C5) -------------------
typedef enum {
    SS_MODE_SILENT_RX     = 0, // PASSIVE only — fully dark. Boot default.
    SS_MODE_TX_DETECTABLE = 1, // ACTIVE_CONNECT + ACTIVE_TRANSMIT unlocked (still per-op gated)
} ss_mode_t;

// mode bitmask for the capability registry's allowed_modes field
typedef uint8_t ss_mode_mask_t;
#define SS_MM_SILENT   (1u << SS_MODE_SILENT_RX)
#define SS_MM_TX       (1u << SS_MODE_TX_DETECTABLE)
#define SS_MM_ANY      (SS_MM_SILENT | SS_MM_TX)

// ---- capability class -------------------------------------------------------
typedef enum {
    SS_CLS_PASSIVE        = 0, // receive-only, no token
    SS_CLS_ACTIVE_CONNECT = 1, // makes a connection; detectable; token + TX mode
    SS_CLS_ACTIVE_TRANSMIT= 2, // emits RF; token + TX mode + armed
} ss_cap_class_t;

// ---- on-wire frame header (packed) ------------------------------------------
// Header is fixed 7 bytes; CRC16 trails the payload.
#if defined(__GNUC__)
#  define SS_PACKED __attribute__((packed))
#else
#  define SS_PACKED
#endif

typedef struct SS_PACKED {
    uint8_t  sof;      // SS_FRAME_SOF
    uint8_t  ver;      // SS_PROTO_VERSION
    uint8_t  type;     // ss_type_t
    uint8_t  seq;      // rolling; RESP echoes REQ seq; EVENT uses 0
    uint8_t  flags;    // SS_FLAG_*
    uint16_t len;      // payload length, little-endian
} ss_frame_hdr_t;      // sizeof == 7

// ---- CRC16-CCITT (poly 0x1021, init 0xFFFF) over ver..end-of-payload --------
// Defined inline so both firmwares use the identical implementation.
// One byte at a time (for streaming parsers); ss_crc16 below is the same thing in bulk.
static inline uint16_t ss_crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (uint8_t b = 0; b < 8; ++b)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                              : (uint16_t)(crc << 1);
    return crc;
}
#define SS_CRC16_INIT 0xFFFFu

static inline uint16_t ss_crc16(const uint8_t* p, uint16_t n) {
    uint16_t crc = SS_CRC16_INIT;
    for (uint16_t i = 0; i < n; ++i) crc = ss_crc16_update(crc, p[i]);
    return crc;
}
