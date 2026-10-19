// SPDX-License-Identifier: GPL-3.0-or-later
// SignalSweep CYD — detection signatures
// Default rule set ported entry-for-entry from upstream
// B0z0dcl0wn/SignalSweep firmware/src/mode_watchers_watch.cpp (GPL-3.0-or-later).
// Provenance notes are condensed from upstream's own comments.

#include "signatures.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

extern DetectionRegistry g_reg;
extern SemaphoreHandle_t g_reg_mutex;
extern QueueHandle_t     g_queue;

#define SIG_RULE(name, cat, oui, mfg, dev, uuid, w, ssid) { name, cat, oui, mfg, dev, uuid, w, ssid }

const Signature SIGNATURES[] = {
    // ── Flock Safety OUIs ────────────────────────────────────────────────────
    // The promiscuous-mode set compiled by OrdoOuroborous / @NitekryDPaul
    // ("GoFlockYourself" extended OUIs). Upstream drops a4:cf:12, 3c:71:bf
    // (Espressif), cc:cc:cc (unassigned), 82:6b:f2 (locally administered) and
    // f8:a2:d6 (field-demoted). OUI-only = W_OUI (30): shared with consumer
    // Wi-Fi modules, so these only list/alert with corroboration (e.g. the
    // Flock wildcard-probe path on Wi-Fi).
    SIG_RULE("Flock Safety MAC", "Flock Safety", "70:c9:4e", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "3c:91:80", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "d8:f3:bc", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "80:30:49", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "b8:35:32", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "14:5a:fc", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "74:4c:a1", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "08:3a:88", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "9c:2f:9d", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "c0:35:32", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "94:08:53", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "e4:aa:ea", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "f4:6a:dd", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "24:b2:b9", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "00:f4:8d", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "d0:39:57", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "e8:d0:fc", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "e0:4f:43", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "b8:1e:a4", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "70:08:94", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "58:8e:81", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "ec:1b:bd", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "58:00:e3", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "90:35:ea", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "5c:93:a2", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "64:6e:69", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "48:27:ea", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "14:b5:cd", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "04:0d:84", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "1c:34:f1", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "38:5b:44", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "94:34:69", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "b4:e3:f9", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "f0:82:c0", "", "", "", 0, ""),
    SIG_RULE("Flock Safety MAC", "Flock Safety", "e0:0a:f6", "", "", "", 0, ""),
    // The one block IEEE registers to Flock Safety itself: beeps alone.
    SIG_RULE("Flock Safety MAC (registered block)", "Flock Safety", "b4:1e:52", "", "", "", 70, ""),
    // SoundThinking / ShotSpotter: sells only to police and cities.
    SIG_RULE("SoundThinking", "SoundThinking", "d4:11:d6", "", "", "", 70, ""),
    // Flock solar battery ("Penguin") GATT services, full 128-bit, exact match.
    SIG_RULE("Flock battery (auth service)",   "Flock Safety", "", "", "", "e8ccbb38-9532-46a8-9fe5-1814df172e6f", 80, ""),
    SIG_RULE("Flock battery (safety service)", "Flock Safety", "", "", "", "20c944c1-add2-42d7-a638-967ee9a26ff6", 80, ""),
    // Flock BLE name keywords ("raven"/"penguin" dropped upstream: too common).
    SIG_RULE("Flock BLE Name", "Flock Safety", "", "", "flock",     "", 0, ""),
    SIG_RULE("Flock BLE Name", "Flock Safety", "", "", "pigvision", "", 0, ""),
    SIG_RULE("Flock BLE Name", "Flock Safety", "", "", "fs_",       "", 0, ""),

    // ── Axon ─────────────────────────────────────────────────────────────────
    // 00:25:df is registered to Axon Enterprise; nearly all of it is police gear.
    SIG_RULE("Axon Enterprise device", "Axon", "00:25:df", "", "", "", 70, ""),
    // Body cams broadcast these SSIDs while pairing (Axon's own admin docs).
    SIG_RULE("Axon Body 2 camera (pairing)", "Axon", "", "", "", "", 0, "AB2-"),
    SIG_RULE("Axon Body 3 camera (pairing)", "Axon", "", "", "", "", 0, "AB3-"),
    SIG_RULE("Axon Body 4 camera (pairing)", "Axon", "", "", "", "", 0, "AB4-"),
    SIG_RULE("Axon device hotspot",          "Axon", "", "", "", "", 0, "AXON-"),
    SIG_RULE("Axon Signal System", "Axon", "", "", "", "fe6c", 0, ""),
    SIG_RULE("Axon Signal System", "Axon", "", "", "", "fe6d", 0, ""),

    // ── Fleet / infrastructure (OUI-only, weight 30: corroboration only) ─────
    SIG_RULE("Cradlepoint Router", "Fleet / Infrastructure", "00:30:44", "", "", "", 0, ""),
    SIG_RULE("Peplink Router",     "Fleet / Infrastructure", "00:1a:dd", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "00:a0:d5", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "28:a3:31", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "50:13:9d", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "64:ce:6e", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "84:db:2f", "", "", "", 0, ""),
    SIG_RULE("Sierra Wireless Infrastructure", "Fleet / Infrastructure", "cc:93:4a", "", "", "", 0, ""),

    // ── Police / ALPR-only vendors: beep on the address alone (70) ───────────
    SIG_RULE("Genetec (AutoVu / Sharp)", "Genetec ALPR", "00:0a:b1", "", "", "", 70, ""),
    SIG_RULE("Genetec (AutoVu / Sharp)", "Genetec ALPR", "00:bf:15", "", "", "", 70, ""),
    SIG_RULE("Genetec (AutoVu / Sharp)", "Genetec ALPR", "0c:bf:15", "", "", "", 70, ""),
    SIG_RULE("Neology (ALPR / tolling)", "Neology ALPR", "00:17:3d", "", "", "", 70, ""),
    SIG_RULE("WatchGuard Video camera",  "WatchGuard body cam",  "00:1d:96", "", "", "", 70, ""),
    SIG_RULE("Digital Ally camera",      "Digital Ally body cam","00:23:bd", "", "", "", 70, ""),

    // ── Business security vendors: listed, never beeping (60) ────────────────
    // Upstream files these under "All" only. They map to our UNKNOWN type,
    // so they are matched but not shown yet (see threat_from_category).
    SIG_RULE("Ubicquia (streetlight node)", "Ubicquia",           "94:7b:be", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "00:04:7d", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "00:18:85", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "00:1f:92", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "4c:cc:34", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "b8:e2:8c", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "10:74:6f", "", "", "", 60, ""),
    SIG_RULE("Motorola Solutions device",   "Motorola Solutions", "9c:86:2b", "", "", "", 60, ""),
    SIG_RULE("Verkada camera",              "Verkada",            "e0:a7:00", "", "", "", 60, ""),
    SIG_RULE("Avigilon Alta device",        "Avigilon Alta",      "70:1a:d5", "", "", "", 60, ""),
    SIG_RULE("Axis Communications camera",  "Axis Communications","00:40:8c", "", "", "", 60, ""),
    SIG_RULE("Axis Communications camera",  "Axis Communications","b8:a4:4f", "", "", "", 60, ""),
    SIG_RULE("Axis Communications camera",  "Axis Communications","ac:cc:8e", "", "", "", 60, ""),
    SIG_RULE("Axis Communications camera",  "Axis Communications","e8:27:25", "", "", "", 60, ""),

    // ── Smart glasses (counted like a body cam) ──────────────────────────────
    // Ray-Ban/Oakley Meta needs BOTH the Luxottica company ID and Meta's fd5f
    // UUID: each alone is a false-positive magnet (Quest headsets).
    SIG_RULE("Ray-Ban / Oakley Meta glasses", "Smart glasses", "", "0x0D53", "", "fd5f", 70, ""),
    SIG_RULE("Snap Spectacles",               "Smart glasses", "", "0x03C2", "", "",     70, ""),

    // ── Attack gear (only while Attack detection is on) ──────────────────────
    SIG_RULE("Flipper Zero (service UUID)", HACKING_GEAR_CATEGORY, "", "", "", "3081", 80, ""),
    SIG_RULE("Flipper Zero (service UUID)", HACKING_GEAR_CATEGORY, "", "", "", "3082", 80, ""),
    SIG_RULE("Flipper Zero (service UUID)", HACKING_GEAR_CATEGORY, "", "", "", "3083", 80, ""),
    SIG_RULE("STM32WB address (Flipper Zero uses this)", HACKING_GEAR_CATEGORY, "80:e1:26", "", "", "", 60, ""),
    SIG_RULE("STM32WB address (Flipper Zero uses this)", HACKING_GEAR_CATEGORY, "80:e1:27", "", "", "", 60, ""),
    SIG_RULE("Flipper Devices (company ID)", HACKING_GEAR_CATEGORY, "", "0x0E29", "", "", 70, ""),
    SIG_RULE("Flipper Devices (MAC)",        HACKING_GEAR_CATEGORY, "0c:fa:22", "", "", "", 70, ""),
    SIG_RULE("Name contains Flipper",        HACKING_GEAR_CATEGORY, "", "", "flipper", "", 60, ""),
    SIG_RULE("Pineapple management AP",      HACKING_GEAR_CATEGORY, "", "", "", "", 70, "Pineapple_"),
    SIG_RULE("OUI-Spy selector AP",          HACKING_GEAR_CATEGORY, "", "", "", "", 70, "oui-spy"),
    SIG_RULE("OUI-Spy Detector AP",          HACKING_GEAR_CATEGORY, "", "", "", "", 70, "snoopuntothem"),
    SIG_RULE("OUI-Spy Foxhunter AP",         HACKING_GEAR_CATEGORY, "", "", "", "", 70, "foxhunter"),
    SIG_RULE("OUI-Spy capture AP (PCAP / BLE Sniff)", HACKING_GEAR_CATEGORY, "", "", "", "", 70, "ouispy-"),
    SIG_RULE("'pwned' SSID (ESP deauther default)",   HACKING_GEAR_CATEGORY, "", "", "", "", 60, "pwned"),

    // ══ CYD additions (not upstream) — sourced, same weighting policy ═══════
    // Drone SSIDs. Tello firmware forces the "TELLO-" prefix even when the
    // network is renamed (DJI/Ryze forum; gobot & gophercon Tello docs).
    // ANAFI: "ANAFI-<serial>" seen in the SkyController3 API (parrotpilots.com);
    // "Anafi_<serial>" per Parrot's developer forum. SSID default weight (80).
    SIG_RULE("DJI Tello drone (AP)",   "Drone", "", "", "", "", 0, "TELLO-"),
    SIG_RULE("Parrot ANAFI drone (AP)","Drone", "", "", "", "", 0, "ANAFI-"),
    SIG_RULE("Parrot ANAFI drone (AP)","Drone", "", "", "", "", 0, "Anafi_"),
    // Drone-maker OUIs. DJI and Parrot also sell gimbals, cameras, headsets and
    // car kits, so OUI-only = W_OUI (30): corroboration, never listed alone.
    // DJI: 60:60:1f, 34:d2:62, 48:1c:b9 ("DJI drone IDs are not encrypted").
    // Parrot: a0:14:3d, 00:12:1c, 00:26:7e, 90:03:b7 (IEEE, via py-minidrone);
    // 90:3a:e6 seen as an ANAFI's Wi-Fi MAC (parrotpilots.com capture).
    SIG_RULE("DJI device (MAC)",    "Drone", "60:60:1f", "", "", "", 0, ""),
    SIG_RULE("DJI device (MAC)",    "Drone", "34:d2:62", "", "", "", 0, ""),
    SIG_RULE("DJI device (MAC)",    "Drone", "48:1c:b9", "", "", "", 0, ""),
    SIG_RULE("Parrot device (MAC)", "Drone", "a0:14:3d", "", "", "", 0, ""),
    SIG_RULE("Parrot device (MAC)", "Drone", "00:12:1c", "", "", "", 0, ""),
    SIG_RULE("Parrot device (MAC)", "Drone", "00:26:7e", "", "", "", 0, ""),
    SIG_RULE("Parrot device (MAC)", "Drone", "90:03:b7", "", "", "", 0, ""),
    SIG_RULE("Parrot device (MAC)", "Drone", "90:3a:e6", "", "", "", 0, ""),
    // Hak5 vanity prefix used by the WiFi Pineapple MK7 (pinescan / sloth
    // tool-fingerprint research). Listed, never beeps (60): one field only.
    SIG_RULE("Hak5 / WiFi Pineapple (MAC)", HACKING_GEAR_CATEGORY, "00:13:37", "", "", "", 60, ""),

    // ── Trackers (service UUID rules; AirTag/FMDN/SmartTag data in code) ─────
    SIG_RULE("Tile Tracker",     "Tracker", "", "", "", "feed", 0, ""),
    SIG_RULE("Tile Tracker",     "Tracker", "", "", "", "feec", 0, ""),
    SIG_RULE("Samsung SmartTag", "Tracker", "", "", "", "fd5a", 0, ""),
};
const size_t SIGNATURE_COUNT = sizeof(SIGNATURES) / sizeof(SIGNATURES[0]);

// Port of upstream alertCategoryFromName(). Order matters: ALPR before body
// cam, because "cam" appears in categories that name both.
ThreatType threat_from_category(const char* category) {
    if (!category || !category[0]) return ThreatType::UNKNOWN;
    String c = String(category);
    c.toLowerCase();
    if (c == "hacking gear") return ThreatType::ATTACK;
    if (c.indexOf("drone") >= 0 || c.indexOf("remote id") >= 0 || c.indexOf("uas") >= 0)
        return ThreatType::DRONE;
    if (c.indexOf("track") >= 0 || c.indexOf("airtag") >= 0 || c.indexOf("tile") >= 0 ||
        c.indexOf("tag") >= 0 || c.indexOf("beacon") >= 0)
        return ThreatType::TRACKER;
    if (c.indexOf("flock") >= 0 || c.indexOf("alpr") >= 0 || c.indexOf("plate") >= 0 ||
        c.indexOf("surveil") >= 0 || c.indexOf("soundthinking") >= 0 || c.indexOf("shotspotter") >= 0)
        return ThreatType::CAMERA;
    if (c.indexOf("body") >= 0 || c.indexOf("axon") >= 0 || c.indexOf("cam") >= 0 ||
        c.indexOf("glasses") >= 0)
        return ThreatType::BODYCAM;
    return ThreatType::UNKNOWN;   // upstream ALERT_GENERIC
}

bool sig_gated_off(const Signature& s, const Settings& cfg) {
    return !cfg.detect_attack && strcmp(s.category, HACKING_GEAR_CATEGORY) == 0;
}

bool type_enabled(ThreatType t, const Settings& cfg) {
    switch (t) {
        case ThreatType::DRONE:   return cfg.detect_drone;
        case ThreatType::TRACKER: return cfg.detect_tracker;
        case ThreatType::CAMERA:  return cfg.detect_camera;
        case ThreatType::BODYCAM: return cfg.detect_bodycam;
        case ThreatType::ATTACK:  return cfg.detect_attack;
        default:                  return false;
    }
}

bool oui_matches(const uint8_t mac[6], const char* oui) {
    unsigned a, b, c;
    if (!oui || sscanf(oui, "%x:%x:%x", &a, &b, &c) != 3) return false;
    return mac[0] == a && mac[1] == b && mac[2] == c;
}

uint16_t parse_mfg_id(const char* s) {
    return (uint16_t)strtoul(s, NULL, 0);   // handles "0x0D53" and decimal
}

ScanStats g_stats = {};

void stats_print() {
    static ScanStats last = {};
    ScanStats n = g_stats;
    Serial.printf("[stats 5s] BLE adv=%u hit=%u drop=%u | WiFi mgmt=%u hit=%u drop=%u | "
                  "low=%u busy=%u new=%u | listed=%d\n",
        (unsigned)(n.ble_adv - last.ble_adv), (unsigned)(n.ble_hit - last.ble_hit),
        (unsigned)(n.ble_drop - last.ble_drop), (unsigned)(n.wifi_mgmt - last.wifi_mgmt),
        (unsigned)(n.wifi_hit - last.wifi_hit), (unsigned)(n.wifi_drop - last.wifi_drop),
        (unsigned)(n.post_low - last.post_low), (unsigned)(n.post_lock - last.post_lock),
        (unsigned)(n.post_new - last.post_new), g_reg.count);
    last = n;
}

void post_detection(Detection& d, int confidence, int bestWeight) {
    if (confidence > 100) confidence = 100;
    if (confidence < CONF_LIST_MIN) { g_stats.post_low++; return; }
    d.confidence  = (uint8_t)confidence;
    d.best_weight = (uint8_t)(bestWeight > 100 ? 100 : bestWeight);
    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(5)) != pdTRUE) { g_stats.post_lock++; return; }
    int before = g_reg.count;
    int idx = g_reg.upsert(d);
    bool isNew = g_reg.count > before;
    bool alert = false;
    if (idx >= 0) {
        Detection& r = g_reg.items[idx];
        // Once per appearance: the flag clears when the entry is pruned.
        if (!r.alerted && r.best_weight >= CONF_ALERT_MIN && !r.muted) {
            r.alerted = true;
            alert = true;
        }
    }
    xSemaphoreGive(g_reg_mutex);
    if (isNew) {
        g_stats.post_new++;
        char m[20]; d.mac_str(m);
        Serial.printf("[det] NEW %s '%s' %s conf=%d best=%d rssi=%d %s\n",
            d.proto == PROTO_WIFI ? "WiFi" : "BLE", d.name, m,
            d.confidence, d.best_weight, d.rssi, alert ? "ALERT" : "listed");
    }
    if (alert) xQueueSend(g_queue, &idx, 0);
}
