// SPDX-License-Identifier: GPL-3.0-or-later
// link.cpp — link state machine, idle auto-off and token. See link.h.
#include "link.h"
#include <Preferences.h>

static const uint8_t STEPS[] = { 5, 15, 30, 60 };
static const int     NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));

static LinkState s_state = LINK_OFF;             // always OFF at boot
static uint32_t  s_deadline = 0;                 // millis() at which the idle timer fires (valid while not OFF)
static int       s_step = -1;                    // index into STEPS, loaded on first use
static char      s_token[8] = {};

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
bool        link_radio_active(void) { return false; }                   // stage 1: no radio

uint8_t link_idle_minutes(void) { load_step(); return STEPS[s_step]; }

void link_idle_step(int dir) {
    load_step();
    s_step = constrain(s_step + dir, 0, NSTEPS - 1);
    Preferences p; p.begin("sslink", false);
    p.putUChar("idle", STEPS[s_step]);
    p.end();
    if (s_state != LINK_OFF) s_deadline = millis() + (uint32_t)STEPS[s_step] * 60000UL;   // the new limit counts from now
}

void link_set(LinkState s) {
    if (s == s_state) return;
    s_state = s;
    if (s != LINK_OFF) s_deadline = millis() + (uint32_t)link_idle_minutes() * 60000UL;
    Serial.printf("[link] state %s\n", link_state_name(s));
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
