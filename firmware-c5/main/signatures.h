// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SignalSweep C5 — detection signatures
// Ported from upstream B0z0dcl0wn/SignalSweep firmware/src/mode_watchers_watch.cpp
// (GPL-3.0-or-later). Weights, thresholds and rules are upstream's; see the
// comments in signatures.cpp for each rule's provenance.

#include <stddef.h>
#include <stdint.h>
#include "detection.h"

#define HACKING_GEAR_CATEGORY "Hacking gear"

// Confidence weights (upstream values)
#define W_OUI            30
#define W_MFG            45
#define W_NAME           70
#define W_UUID           70
#define W_WIFI_OUI       30
#define W_WIFI_IE        30
#define W_WIFI_SSID      80
#define W_WIFI_PROBE     70
#define W_WIFI_IE_SIG    80
#define W_DRONE          90
#define W_TRACKER        80
#define W_PENGUIN        80

// Listing vs alerting (upstream semantics):
//   list   when the SUM of matched signals >= CONF_LIST_MIN
//   alert  when the STRONGEST SINGLE signal >= CONF_ALERT_MIN
#define CONF_LIST_MIN    60
#define CONF_ALERT_MIN   70

// Detection toggles. All on by default; the CYD will push changes in a later milestone.
struct DetectCfg {
    bool detect_drone = true, detect_tracker = true, detect_camera = true,
         detect_bodycam = true, detect_attack = true;
};
extern DetectCfg g_cfg;

// One rule. Every non-empty condition must match (AND). If weight > 0 it
// replaces the per-condition sum.
struct Signature {
    const char* name;
    const char* category;
    const char* oui;     // "aa:bb:cc" or ""
    const char* mfg;     // "0x0D53" or ""
    const char* dev;     // device-name substring (case-insensitive) or ""
    const char* uuid;    // exact 16-bit ("fd5f") or full 128-bit UUID, or ""
    int         weight;
    const char* ssid;    // Wi-Fi SSID prefix or ""
};

extern const Signature SIGNATURES[];
extern const size_t    SIGNATURE_COUNT;

// Port of upstream alertCategoryFromName(), mapped onto our 5 ThreatTypes.
// Hacking gear -> ATTACK. Anything upstream calls GENERIC -> UNKNOWN (not shown).
ThreatType threat_from_category(const char* category);

// Upstream gates attack-gear rules on the Attack toggle.
bool sig_gated_off(const Signature& s, const DetectCfg& cfg);

// Is detection of this type enabled in Settings?
bool type_enabled(ThreatType t, const DetectCfg& cfg);

// MAC prefix compare against "aa:bb:cc".
bool oui_matches(const uint8_t mac[6], const char* oui);

// Parse "0x0D53" / "3411" style company IDs.
uint16_t parse_mfg_id(const char* s);

