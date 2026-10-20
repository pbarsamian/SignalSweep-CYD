// SPDX-License-Identifier: GPL-3.0-or-later
// sched.h — who gets the radio: Wi-Fi capture, BLE scan, or both.
//
// Wi-Fi and BLE share the C5's one 2.4 GHz radio, so running both at once costs each of them frames
// (BLE at a 50 % scan window roughly halves the Wi-Fi management-frame rate). The scheduler decides
// how to split the time. Passive only: it only pauses and resumes receive paths and never transmits.
//
//   CONCURRENT  both run all the time; the BLE scan window is `ble_pct` percent of its 100 ms interval,
//               and Wi-Fi gets what BLE leaves. (50 % was the fixed behaviour before the scheduler.)
//               Measured: 20 % -> Wi-Fi +54 %, BLE -61 %; 80 % -> Wi-Fi -65 %, BLE +66 % (vs 50 %).
//   SLICED      the radio alternates: `wifi_ms` of Wi-Fi only, then `ble_ms` of BLE only. Each side
//               has the radio to itself while it runs, but is blind while the other runs.
//   lock        focus mode takes over: LOCK_WIFI = Wi-Fi only, LOCK_BLE = BLE only at full duty.
//
// Charge mode is not the scheduler's business: when the radios are switched off for charging they stay
// off, whatever the scheduler wants (it only decides between running radios).
#pragma once
#include <stdint.h>

enum SchedMode : uint8_t { SCHED_CONCURRENT = 0, SCHED_SLICED = 1 };
enum SchedLock : uint8_t { SCHED_LOCK_NONE = 0, SCHED_LOCK_WIFI = 1, SCHED_LOCK_BLE = 2 };

void sched_start(void);                                   // after the Wi-Fi and BLE radios are initialised
// -1 leaves a value unchanged. Values are clamped: ble_pct 10..90, slices 500..30000 ms.
void sched_set(int mode, int ble_pct, int wifi_ms, int ble_ms);
void sched_lock(uint8_t lock);                            // SCHED_LOCK_*; NONE hands control back
void sched_log(void);                                     // one status line on the console
uint8_t sched_run_mask(void);                             // what is running now: bit0 Wi-Fi, bit1 BLE
uint8_t sched_lock_state(void);                           // SCHED_LOCK_*
