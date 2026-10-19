// ss_capabilities.h — SignalSweep-CYD capability registry (SHARED).
// The single declarative list that both (a) the CYD's gate logic and (b) the
// CYD's MCP tools/list are generated from — so the tool surface and the gate
// can never disagree. The C5 also uses it to self-check an incoming opcode's
// class before executing. See PROTOCOL.md §4–§6.
#pragma once
#include "ss_protocol.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One registry row.
typedef struct {
    ss_opcode_t     opcode;
    const char*     id;             // stable id == MCP tool name
    ss_cap_class_t  cls;
    bool            requires_token;
    ss_mode_mask_t  allowed_modes;
    const char*     disclaimer;     // shown in the per-op pop-up; NULL if none
    const char*     mcp_desc;       // one-line description for tools/list
} ss_cap_t;

// ---- THE LIST ---------------------------------------------------------------
// X(opcode, id, class, requires_token, allowed_modes, disclaimer, mcp_desc)
//
// PASSIVE  -> no token, any mode.
// ACTIVE_* -> token required, TX_DETECTABLE only, each carries a disclaimer.
//
// TX slots are intentionally present-but-empty: the framework gates and routes
// them, but the handlers are user-supplied (PROTOCOL.md §7). Their disclaimers
// are written to be shown verbatim before a human authorizes.
#define SS_CAP_LIST(X)                                                          \
  X(SS_OP_SCAN_WIFI,          "scan_wifi",          SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Listen-only 2.4/5 GHz Wi-Fi scan. No probe requests are sent.")         \
  X(SS_OP_SCAN_BLE,           "scan_ble",           SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Passive BLE scan. No scan-requests are sent.")                          \
  X(SS_OP_LIST_DETECTIONS,    "list_detections",    SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Return the current detection table.")                                   \
  X(SS_OP_GET_DEVICE_PROFILE, "get_device_profile", SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Deep passive profile: OUI/company, address-type, WPS, Continuity, IEs.")\
  X(SS_OP_REMOTE_ID_DECODE,   "remote_id_decode",   SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Decode ASTM F3411 drone Remote ID (BLE, Wi-Fi beacon, Wi-Fi NAN).")     \
  X(SS_OP_FOCUS_TARGET,       "focus_target",       SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Lock the radio to one target's channel / full-time BLE listen.")        \
  X(SS_OP_SUBSCRIBE_EVENTS,   "subscribe_events",   SS_CLS_PASSIVE,         false, SS_MM_ANY, NULL,                                                             \
      "Stream new detections and RSSI ticks as events.")                       \
  X(SS_OP_GATT_CONNECT_READ,  "gatt_connect_read",  SS_CLS_ACTIVE_CONNECT,  true,  SS_MM_TX,                                                                    \
      "This CONNECTS to the target device. The target can see the connection "  \
      "— you will no longer be silent. Authorize?",                             \
      "Connect to a BLE target and enumerate its GATT services/characteristics.")\
  X(SS_OP_DEVICE_INFO_READ,   "device_info_read",   SS_CLS_ACTIVE_CONNECT,  true,  SS_MM_TX,                                                                    \
      "This CONNECTS to the target to read its Device Information. The target "  \
      "can see the connection. Authorize?",                                     \
      "Connect and read the BLE Device Information Service.")                   \
  /* ---- ACTIVE_TRANSMIT registry slots — HANDLERS NOT PROVIDED (PROTOCOL.md §7) ---- */ \
  X(SS_OP_TX_SLOT_A,          "tx_slot_a",          SS_CLS_ACTIVE_TRANSMIT, true,  SS_MM_TX,                                                                    \
      "This TRANSMITS and makes this device detectable and may be regulated or " \
      "illegal to operate in your jurisdiction. Authorize?",                    \
      "User-supplied transmit capability (unassigned).")                       \
  X(SS_OP_TX_SLOT_B,          "tx_slot_b",          SS_CLS_ACTIVE_TRANSMIT, true,  SS_MM_TX,                                                                    \
      "This TRANSMITS and makes this device detectable and may be regulated or " \
      "illegal to operate in your jurisdiction. Authorize?",                    \
      "User-supplied transmit capability (unassigned).")                       \
  X(SS_OP_TX_SLOT_C,          "tx_slot_c",          SS_CLS_ACTIVE_TRANSMIT, true,  SS_MM_TX,                                                                    \
      "This TRANSMITS and makes this device detectable and may be regulated or " \
      "illegal to operate in your jurisdiction. Authorize?",                    \
      "User-supplied transmit capability (unassigned).")                       \
  X(SS_OP_TX_SLOT_D,          "tx_slot_d",          SS_CLS_ACTIVE_TRANSMIT, true,  SS_MM_TX,                                                                    \
      "This TRANSMITS and makes this device detectable and may be regulated or " \
      "illegal to operate in your jurisdiction. Authorize?",                    \
      "User-supplied transmit capability (unassigned).")

// Build the table in exactly one .c/.cpp per firmware:
//   #define SS_CAP_ROW(op,id,cls,tok,modes,disc,desc) {op,id,cls,tok,modes,disc,desc},
//   const ss_cap_t SS_CAP_TABLE[] = { SS_CAP_LIST(SS_CAP_ROW) };
//   const size_t   SS_CAP_COUNT   = sizeof(SS_CAP_TABLE)/sizeof(SS_CAP_TABLE[0]);
extern const ss_cap_t SS_CAP_TABLE[];
extern const size_t   SS_CAP_COUNT;

// Lookup + gate helpers (implement once, shared logic described in PROTOCOL.md §4).
const ss_cap_t* ss_cap_find(ss_opcode_t op);          // NULL if unknown
// Returns SS_OK if callable, else SS_ERR_MODE / SS_ERR_TOKEN / SS_ERR_OPCODE.
ss_err_t ss_cap_check(ss_opcode_t op, ss_mode_t mode, bool token_valid);

#ifdef __cplusplus
}
#endif
