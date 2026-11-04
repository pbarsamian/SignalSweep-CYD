// SPDX-License-Identifier: GPL-3.0-or-later
// link.h — the CYD's Wi-Fi link state (issue #23, design in PROTOCOL.md §6a). Three states, one at a time:
// OFF (the only state at boot), JOIN (station) and AP. The CYD owns this value; the header pill and the LINK
// screen read it from here, so they can never disagree.
//
// STAGE 1: state, idle auto-off and the token only. No radio is started yet, so nothing transmits.
// Stage 2 hooks AP into link_set(); stage 3 hooks JOIN.
#pragma once
#include <Arduino.h>

enum LinkState : uint8_t { LINK_OFF = 0, LINK_JOIN = 1, LINK_AP = 2 };

LinkState   link_state(void);
const char* link_state_name(LinkState s);
void        link_set(LinkState s);          // going OFF is always allowed and immediate
bool        link_radio_active(void);        // true only while a radio really is transmitting (false in stage 1)

// Idle auto-off: the link turns itself off after N minutes with no MCP request. Steps 5/15/30/60, default 15,
// saved in NVS. There is deliberately no "never".
uint8_t     link_idle_minutes(void);
void        link_idle_step(int dir);        // -1 / +1 along the steps; saved
void        link_activity(void);            // an MCP request arrived: restart the idle clock
uint32_t    link_idle_left_s(void);         // 0 when the link is off
bool        link_tick(uint32_t now_ms);     // returns true on the call that auto-turned the link off

// Bearer token for the MCP endpoint (stage: 9b). Six random digits, new at every boot; link_new_token() re-rolls it.
const char* link_token(void);
void        link_new_token(void);
