// SPDX-License-Identifier: GPL-3.0-or-later
// odid_selftest.h — boot-time check of the Remote ID decode path (no drone needed).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encodes a known ASTM F3411 message pack, decodes it through the same helpers the Wi-Fi and BLE
// scanners use, and checks every field. Logs one line per case; true if all pass.
bool odid_selftest(void);

#ifdef __cplusplus
}
#endif
