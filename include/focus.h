// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SignalSweep CYD — focus mode
// When a detection's info view is open, sweeping stops and the radio works that
// one target. Deep fields are collected here, for the focused device only.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define FOCUS_NONE 0
#define FOCUS_BLE  1
#define FOCUS_WIFI 2
#define FOCUS_MAX_VIE 8

struct FocusProfile {
    bool     active = false;
    uint8_t  mac[6] = {};
    uint8_t  radio = FOCUS_NONE;
    uint8_t  channel = 0;
    uint32_t started_ms = 0;
    // BLE
    uint32_t adv_count = 0;
    uint32_t last_adv_ms = 0;
    uint32_t adv_int_min = 0;          // ms, smallest gap seen between adverts
    uint8_t  raw[62] = {};             // latest advertising payload (+ scan response)
    uint8_t  raw_len = 0;
    // WiFi
    uint32_t frames = 0, beacons = 0, probe_resps = 0, probe_reqs = 0, other_mgmt = 0;
    uint8_t  ie_map[32] = {};          // bitmap of element IDs seen
    uint8_t  vie[FOCUS_MAX_VIE][4] = {};// vendor element OUI + type
    uint8_t  vie_count = 0;
    bool     has_wps = false;
    char     wps_manuf[33] = {}, wps_model[33] = {}, wps_model_num[33] = {};
    char     wps_serial[33] = {}, wps_devname[33] = {};
    uint16_t wps_cat = 0;
    uint8_t  wps_state = 0;            // 1 unconfigured, 2 configured
    uint8_t  ht_streams = 0;
};

extern FocusProfile g_focus;

void focus_init();
void focus_start(const uint8_t mac[6], uint8_t radio, uint8_t channel);
void focus_stop();
bool focus_target(const uint8_t mac[6], uint8_t radio);   // fast check from callbacks
bool focus_copy(FocusProfile& out);                       // snapshot for the UI

// Called from the scanners for frames from the focused device
void focus_note_ble(const uint8_t* payload, size_t len);
void focus_note_wifi(uint8_t subtype, const uint8_t* frame, int length);
