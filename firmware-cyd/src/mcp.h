// SPDX-License-Identifier: GPL-3.0-or-later
// mcp.h — the CYD's read-only MCP server (issue #25, design in PROTOCOL.md §6a/§6b). JSON-RPC 2.0 over HTTP:
// POST /mcp, one request per connection, the bearer token checked BEFORE any body is read, bodies capped at 2 KB.
// It runs only while the Wi-Fi link is up (AP, or JOIN once connected) and stops when the link goes down.
// Tools come from the shared capability registry (PASSIVE rows that have a handler here), so the tool list and
// the gate cannot disagree.
#pragma once
#include <Arduino.h>
#include "status_ui.h"

void mcp_init(const C5Status* c5);       // once, at setup
void mcp_tick(uint32_t now_ms);          // every loop: starts/stops the listener with the link, serves one request at a time
bool mcp_running(void);

// On-demand polling: true for a short while after a tool asked for data the C5 is not already being polled for.
// main.cpp ORs these into the polls the screens drive. The profile slot is shared with the INFO screen, which wins.
bool mcp_wants_aps(void);
bool mcp_wants_profile(char* mac_out);   // copies the MAC text (18 bytes) when true
