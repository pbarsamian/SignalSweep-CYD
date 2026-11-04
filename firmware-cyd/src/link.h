// SPDX-License-Identifier: GPL-3.0-or-later
// link.h — the CYD's Wi-Fi link state (issue #23, design in PROTOCOL.md §6a). Three states, one at a time:
// OFF (the only state at boot), JOIN (station) and AP. The CYD owns this value; the header pill and the LINK
// screen read it from here, so they can never disagree.
//
// STAGE 2: AP is real (WPA2-PSK soft-AP, random SSID, PSK and channel at every start). JOIN is still only a
// state: stage 3 starts its radio. Nothing here serves HTTP yet (issue #9b).
#pragma once
#include <Arduino.h>

enum LinkState : uint8_t { LINK_OFF = 0, LINK_JOIN = 1, LINK_AP = 2 };

LinkState   link_state(void);
const char* link_state_name(LinkState s);
bool        link_set(LinkState s);          // going OFF is always allowed and immediate; false if the radio would not start (state is then OFF)
bool        link_radio_active(void);        // true only while a radio really is transmitting

// JOIN: link_set(LINK_JOIN) only selects the state (radio stays off until a network is chosen). link_join() starts
// the station radio on a network; it returns at once and the result shows up in link_join_state() / link_take_notice().
// A failed or dropped join goes to LINK_OFF with a notice; there is no auto-reconnect. The password is never stored here.
enum LinkJoin : uint8_t { LJ_NONE = 0, LJ_CONNECTING = 1, LJ_CONNECTED = 2 };
bool        link_join(const char* ssid, const char* pass);   // pass may be NULL or "" for an open network
LinkJoin    link_join_state(void);
const char* link_join_ssid(void);
const char* link_join_ip(void);
int         link_join_rssi(void);
const char* link_take_notice(void);                          // a message to show once (join failed, ...), or NULL

// The AP's details, valid while link_state() == LINK_AP. Random at every start; the PSK is shown on the LINK screen
// only, never printed to serial.
const char* link_ap_ssid(void);
const char* link_ap_psk(void);
uint8_t     link_ap_channel(void);
int         link_ap_clients(void);
const char* link_ap_ip(void);

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
