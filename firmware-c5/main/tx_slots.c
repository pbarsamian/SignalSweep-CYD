// SPDX-License-Identifier: GPL-3.0-or-later
#include "tx_slots.h"

// Intentionally empty. See tx_slots.h and BUILD_MANIFEST.md section 0.
ss_err_t tx_slot_run(ss_opcode_t op, const uint8_t* args, size_t n) {
    (void)op; (void)args; (void)n;
    return SS_ERR_UNIMPL;
}
