// SPDX-License-Identifier: GPL-3.0-or-later
// tx_slots.h — ACTIVE_TRANSMIT registry slots A..D.
//
// BUILD_MANIFEST.md section 0: these are typed, gated, EMPTY slots. The framework
// routes to them, gates them, and demands a human-confirmed token, but no handler
// body is provided in this repository. The owner may wire in their own or a
// GPL-compatible module; the consent architecture treats it identically.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ss_protocol.h"

// Called only after the gate (mode + token) has passed. Always SS_ERR_UNIMPL here.
ss_err_t tx_slot_run(ss_opcode_t op, const uint8_t* args, size_t n);
