// SPDX-License-Identifier: GPL-3.0-or-later
#include "mode.h"

static ss_mode_t s_mode = SS_MODE_SILENT_RX;   // boot silent

ss_mode_t mode_get() { return s_mode; }

const char* mode_name(ss_mode_t m) {
    return m == SS_MODE_TX_DETECTABLE ? "TX-DETECTABLE" : "RX-ONLY";
}

void mode_force_silent() { s_mode = SS_MODE_SILENT_RX; }
