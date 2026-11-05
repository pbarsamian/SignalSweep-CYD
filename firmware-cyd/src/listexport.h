// SPDX-License-Identifier: GPL-3.0-or-later
// listexport.h — save the CYD's detection list to the microSD as a CSV and a JSON file (issue #15).
//
// The record layout is "Detection record, schema v1" in PROTOCOL.md, the same one the MCP read tool will return.
// Nothing here transmits and nothing but the schema's fields is written (no payloads, no frame bytes).
// Runs in the loop task, the same one that writes captures and alerts.csv, so the SD bus is never shared.
#pragma once
#include <Arduino.h>
#include "model.h"

enum ListMacMode : uint8_t { LMAC_FULL = 0, LMAC_OUI = 1, LMAC_NONE = 2 };

// Writes /signalsweep/list_NNN.csv and list_NNN.json (NNN = first number where neither exists; never overwrites).
// rows: the detections to save, in order. filter: a short description of what the list view was showing.
// On success msg holds the file stem ("list_003: 12 detections"); on failure the reason. Returns success.
bool listexport_save(const Det* const* rows, int n, ListMacMode mode, const char* filter, char* msg, size_t msgsz);

// The same record writers, for any Print (the MCP server uses them): the schema envelope and one detection record, so the
// SD file and the MCP answer cannot drift apart. They return the bytes written (0 = failure). mode is clamped to a valid value.
const char* listexport_mac_mode_name(ListMacMode m);
size_t listexport_json_head(Print& o, ListMacMode mode, const char* filter, int count);   // ends inside the "detections" array
size_t listexport_json_record(Print& o, const Det& d, ListMacMode mode, bool first);       // first: no leading comma
