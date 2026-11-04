// SPDX-License-Identifier: GPL-3.0-or-later
// link.cpp — link state machine, idle auto-off and token. See link.h.
#include "link.h"
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>

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
static LinkJoin  s_join = LJ_NONE;               // the station side of LINK_JOIN
static char      s_jssid[33] = {}, s_jip[16] = {};
static uint32_t  s_jdeadline = 0;                // millis() at which a join attempt gives up
static char      s_notice[40] = {};              // a message for the UI to toast once
#define JOIN_TIMEOUT_MS 15000
static volatile uint8_t s_reason = 0;            // the station's last disconnect reason (esp_wifi_types.h), 0 = none this attempt
static bool      s_ev_hooked = false;

// WiFi.status() cannot tell a wrong password from a slow join; the disconnect reason can.
static void on_sta_disconnected(arduino_event_id_t, arduino_event_info_t info) {
    s_reason = info.wifi_sta_disconnected.reason;
}

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
bool        link_radio_active(void) { return s_ap_up || s_join != LJ_NONE; }
LinkJoin    link_join_state(void) { return s_join; }
const char* link_join_ssid(void) { return s_jssid; }
const char* link_join_ip(void) { return s_jip; }
int         link_join_rssi(void) { return s_join == LJ_CONNECTED ? WiFi.RSSI() : 0; }
const char* link_take_notice(void) {
    static char out[sizeof(s_notice)];
    if (!s_notice[0]) return nullptr;
    strcpy(out, s_notice);
    s_notice[0] = 0;
    return out;
}
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

// ---- JOIN (station) -------------------------------------------------------------------------------------------
static void sta_stop() {
    if (s_join == LJ_NONE) return;
    MDNS.end();
    WiFi.disconnect(true, true);                                         // radio off, forget the credentials held in RAM
    WiFi.mode(WIFI_OFF);
    s_join = LJ_NONE;
    s_jssid[0] = 0; s_jip[0] = 0;
    Serial.printf("[link] JOIN down, free heap %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
}

bool link_join(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 63)) return false;
    if (s_state != LINK_JOIN) link_set(LINK_JOIN);                       // stops the AP if it was up
    sta_stop();                                                          // a second join replaces the first
    strncpy(s_jssid, ssid, sizeof(s_jssid) - 1);
    s_jssid[sizeof(s_jssid) - 1] = 0;
    uint32_t before = ESP.getFreeHeap();
    if (!s_ev_hooked) { WiFi.onEvent(on_sta_disconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED); s_ev_hooked = true; }
    s_reason = 0;
    WiFi.persistent(false);
    WiFi.setHostname("signalsweep");                                     // before mode(): the station picks it up when it starts
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);                                        // a dropped link goes to OFF; the user taps to rejoin
    WiFi.begin(ssid, (pass && pass[0]) ? pass : nullptr);
    s_join = LJ_CONNECTING;
    s_jdeadline = millis() + JOIN_TIMEOUT_MS;
    link_activity();
    Serial.printf("[link] JOIN start: ssid %s, free heap %u KB\n", s_jssid, (unsigned)(before / 1024));
    return true;
}

bool link_set(LinkState s) {
    if (s == s_state) return true;
    if (s_state == LINK_AP) ap_stop();
    if (s_state == LINK_JOIN) sta_stop();
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
    if (s_join == LJ_CONNECTING || s_join == LJ_CONNECTED) {
        wl_status_t st = WiFi.status();
        const char* fail = nullptr;
        if (s_join == LJ_CONNECTING) {
            if (st == WL_CONNECTED) {
                s_join = LJ_CONNECTED;
                snprintf(s_jip, sizeof(s_jip), "%s", WiFi.localIP().toString().c_str());
                bool mdns = MDNS.begin("signalsweep");
                Serial.printf("[link] JOIN up: ssid %s ip %s rssi %d mdns %s, free heap %u KB (min %u)\n", s_jssid, s_jip, WiFi.RSSI(),
                              mdns ? "ok" : "failed", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024));
            }
            else if (st == WL_NO_SSID_AVAIL || s_reason == WIFI_REASON_NO_AP_FOUND) fail = "network not found";
            else if (st == WL_CONNECT_FAILED || s_reason == WIFI_REASON_AUTH_FAIL || s_reason == WIFI_REASON_MIC_FAILURE ||
                     s_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || s_reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
                fail = "wrong password";
            else if ((int32_t)(now_ms - s_jdeadline) >= 0) {
                static char tmo[28];
                snprintf(tmo, sizeof(tmo), "timed out (reason %u)", (unsigned)s_reason);
                fail = tmo;
            }
        } else if (st != WL_CONNECTED) fail = "connection lost";
        if (fail) {
            Serial.printf("[link] JOIN failed: %s\n", fail);
            snprintf(s_notice, sizeof(s_notice), "join failed: %s", fail);
            link_set(LINK_OFF);
            return false;
        }
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
