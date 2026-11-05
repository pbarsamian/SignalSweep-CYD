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

// Scan (stage 3c): a passive 2.4 GHz scan (no probe requests) for the JOIN view's network list. Only while the state is JOIN
// and not joined. The radio goes dark again when it finishes. Results are RAM only: SSID, strength, open or secured,
// strongest first, one entry per SSID, hidden networks left out (use ADD for those).
enum LinkScan : uint8_t { LS_IDLE = 0, LS_SCANNING = 1, LS_DONE = 2 };
bool        link_scan_start(void);
void        link_scan_stop(void);                            // cancels a scan in progress and forgets the results
LinkScan    link_scan_state(void);
int         link_scan_count(void);
const char* link_scan_ssid(int i);
int         link_scan_rssi(int i);
bool        link_scan_open(int i);

// Saved networks (stage 3b): up to LINK_MAX_NETS, SSID + password in NVS. The password is NOT encrypted at rest and
// never leaves link.cpp except to the Wi-Fi stack. Nothing auto-joins at boot: a saved network joins only when picked.
#define LINK_MAX_NETS 4
int         link_net_count(void);
const char* link_net_ssid(int i);                            // NULL if i is out of range
bool        link_net_save(const char* ssid, const char* pass);   // add, or update the password of a saved SSID; false if full
void        link_net_forget(int i);                          // the list closes the gap
void        link_net_forget_all(void);                       // used by the test hook only
bool        link_net_join(int i);                            // password read from NVS, never returned to the caller

// After a successful join of a network that is not saved (or saved with a different password) the link offers to save
// it. The password is held in RAM until the offer is answered or the link goes down, then wiped.
bool        link_offer_pending(void);                        // the connected network is waiting for SAVE / NO
void        link_offer_answer(bool save);

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

// Bearer token for the MCP endpoint (9b). Ten random characters, new at every boot; link_new_token() re-rolls it.
const char* link_token(void);
void        link_new_token(void);
