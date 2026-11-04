// SPDX-License-Identifier: GPL-3.0-or-later
// link.cpp — link state machine, idle auto-off and token. See link.h.
#include "link.h"
#include <Preferences.h>
#include <WiFi.h>

static const uint8_t STEPS[] = { 5, 15, 30, 60 };
static const int     NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));

static LinkState s_state = LINK_OFF;             // always OFF at boot
static uint32_t  s_deadline = 0;                 // millis() at which the idle timer fires (valid while not OFF)
static int       s_step = -1;                    // index into STEPS, loaded on first use
static char      s_token[8] = {};
static char      s_ssid[12] = {}, s_psk[12] = {}, s_ip[16] = {};
static uint8_t   s_chan = 1;
static bool      s_ap_up = false;
static int       s_clients = 0;

static void load_step() {
    if (s_step >= 0) return;
    Preferences p; p.begin("sslink", true);
    uint8_t m = p.getUChar("idle", 15);
    p.end();
    s_step = 1;
    for (int i = 0; i < NSTEPS; i++) if (STEPS[i] == m) s_step = i;
}

const char* link_state_name(LinkState s) { return s == LINK_JOIN ? "JOIN" : s == LINK_AP ? "AP" : "OFF"; }
LinkState   link_state(void) { return s_state; }
bool        link_radio_active(void) { return s_ap_up; }                 // stage 3 adds JOIN
const char* link_ap_ssid(void) { return s_ssid; }
const char* link_ap_psk(void) { return s_psk; }
uint8_t     link_ap_channel(void) { return s_chan; }
int         link_ap_clients(void) { return s_clients; }
const char* link_ap_ip(void) { return s_ip; }

uint8_t link_idle_minutes(void) { load_step(); return STEPS[s_step]; }

void link_idle_step(int dir) {
    load_step();
    s_step = constrain(s_step + dir, 0, NSTEPS - 1);
    Preferences p; p.begin("sslink", false);
    p.putUChar("idle", STEPS[s_step]);
    p.end();
    if (s_state != LINK_OFF) s_deadline = millis() + (uint32_t)STEPS[s_step] * 60000UL;   // the new limit counts from now
}

// ---- AP -------------------------------------------------------------------------------------------------------
static bool ap_start() {
    static const char AL[] = "abcdefghjkmnpqrstuvwxyz23456789";          // 31 characters, none easily misread
    static const uint8_t CH[3] = { 1, 6, 11 };
    snprintf(s_ssid, sizeof(s_ssid), "ss-%04x", (unsigned)(esp_random() & 0xFFFF));
    for (int i = 0; i < 11; i++) s_psk[i] = (i == 5) ? '-' : AL[esp_random() % (sizeof(AL) - 1)];
    s_psk[11] = 0;
    s_chan = CH[esp_random() % 3];
    uint32_t before = ESP.getFreeHeap();
    WiFi.persistent(false);                                              // never write the credentials to flash
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(s_ssid, s_psk, s_chan, 0, 4);                  // visible SSID, WPA2-PSK, up to 4 clients
    if (!ok) { WiFi.mode(WIFI_OFF); Serial.println("[link] AP failed to start"); return false; }
    snprintf(s_ip, sizeof(s_ip), "%s", WiFi.softAPIP().toString().c_str());
    s_clients = 0;
    s_ap_up = true;
    Serial.printf("[link] AP up: ssid %s ch %u ip %s, free heap %u -> %u KB (min %u)\n", s_ssid, (unsigned)s_chan, s_ip,
                  (unsigned)(before / 1024), (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
    return true;
}

static void ap_stop() {
    if (!s_ap_up) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    s_ap_up = false;
    memset(s_psk, 0, sizeof(s_psk));                                     // the PSK does not outlive the AP
    s_clients = 0;
    Serial.printf("[link] AP down, free heap %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
}

bool link_set(LinkState s) {
    if (s == s_state) return true;
    if (s_state == LINK_AP) ap_stop();
    s_state = s;
    bool ok = true;
    if (s == LINK_AP && !ap_start()) { s_state = LINK_OFF; ok = false; }
    if (s_state != LINK_OFF) s_deadline = millis() + (uint32_t)link_idle_minutes() * 60000UL;
    Serial.printf("[link] state %s\n", link_state_name(s_state));
    return ok;
}

void link_activity(void) {
    if (s_state != LINK_OFF) s_deadline = millis() + (uint32_t)link_idle_minutes() * 60000UL;
}

uint32_t link_idle_left_s(void) {
    if (s_state == LINK_OFF) return 0;
    int32_t left = (int32_t)(s_deadline - millis());
    return left > 0 ? (uint32_t)left / 1000 : 0;
}

bool link_tick(uint32_t now_ms) {
    if (s_ap_up) {
        int n = WiFi.softAPgetStationNum();
        if (n != s_clients) { s_clients = n; Serial.printf("[link] AP clients %d\n", n); }
    }
    if (s_state != LINK_OFF && (int32_t)(now_ms - s_deadline) >= 0) {
        Serial.println("[link] idle timeout");
        link_set(LINK_OFF);
        return true;
    }
    return false;
}

void link_new_token(void) { snprintf(s_token, sizeof(s_token), "%06u", (unsigned)(esp_random() % 1000000u)); }

const char* link_token(void) {
    if (!s_token[0]) link_new_token();
    return s_token;
}
