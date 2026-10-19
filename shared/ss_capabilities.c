// SPDX-License-Identifier: GPL-3.0-or-later
// ss_capabilities.c — expands SS_CAP_LIST into the registry table and implements
// the gate. Compiled into BOTH firmwares (CYD and C5); never copied.
#include "ss_capabilities.h"

#define SS_CAP_ROW(op, id, cls, tok, modes, disc, desc) { op, id, cls, tok, modes, disc, desc },
const ss_cap_t SS_CAP_TABLE[] = { SS_CAP_LIST(SS_CAP_ROW) };
const size_t   SS_CAP_COUNT   = sizeof(SS_CAP_TABLE) / sizeof(SS_CAP_TABLE[0]);

const ss_cap_t* ss_cap_find(ss_opcode_t op) {
    for (size_t i = 0; i < SS_CAP_COUNT; i++)
        if (SS_CAP_TABLE[i].opcode == op) return &SS_CAP_TABLE[i];
    return NULL;
}

// Gate, exactly as PROTOCOL.md section 4:
//   mode not allowed            -> SS_ERR_MODE
//   token required but invalid  -> SS_ERR_TOKEN
// Mode is checked first so a SILENT_RX request for an ACTIVE op is always
// answered "enable TX mode first", never a token error.
ss_err_t ss_cap_check(ss_opcode_t op, ss_mode_t mode, bool token_valid) {
    const ss_cap_t* c = ss_cap_find(op);
    if (!c) return SS_ERR_OPCODE;
    if (!(c->allowed_modes & (1u << (unsigned)mode))) return SS_ERR_MODE;
    if (c->requires_token && !token_valid) return SS_ERR_TOKEN;
    return SS_OK;
}
