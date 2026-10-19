// SPDX-License-Identifier: GPL-3.0-or-later
// mode.h — the ONE authoritative operating mode (invariant 1). The UI pill and the
// capability gate both read it from here; nothing keeps a second copy.
// Boots SILENT_RX, always (invariant 2).
#pragma once
#include "ss_protocol.h"

ss_mode_t   mode_get();
const char* mode_name(ss_mode_t m);

// M2 deliberately has no way to enter TX_DETECTABLE: that transition needs the
// warning pop-up + touch-confirm, which arrive with M4. Going quieter is always
// allowed and immediate (invariant 3).
void mode_force_silent();
