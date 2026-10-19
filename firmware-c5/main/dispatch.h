// SPDX-License-Identifier: GPL-3.0-or-later
// dispatch.h — opcode routing + the C5's independent safety gate.
#pragma once
#include "ss_frame.h"

void dispatch_init(void);
void dispatch_handle(const ss_frame_t* f);   // one valid frame (REQ expected)
void dispatch_tick(void);                    // periodic work: status event, shadow staleness
