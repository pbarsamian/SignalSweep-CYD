// SPDX-License-Identifier: GPL-3.0-or-later
// model.h — the CYD's view of what the C5 has heard: detections and access points, built from
// LIST_DETECTIONS / SCAN_WIFI replies and SS_EVT_DETECTION events. The C5 stays the source of
// truth; this is a cache the UI draws from.
#pragma once
#include <Arduino.h>

#define MODEL_MAX_DETS 32
#define MODEL_MAX_APS  24

struct Det {
    bool     used = false;
    uint8_t  type = 5;                 // T_* in theme.h
    uint8_t  conf = 0, best = 0;
    int8_t   rssi = -100;
    uint8_t  ch = 0, band = 0;         // band: 2 = 2.4 GHz, 5 = 5 GHz, 0 = BLE
    uint8_t  radio = 0;                // 0 wifi, 1 ble, 2 both
    uint16_t seen = 0;
    bool     alerted = false, muted = false;
    uint32_t first_ms = 0, last_ms = 0;
    uint8_t  sweep = 0;                // list-sync generation (see model_ingest_list)
    char     mac[18] = {};
    char     name[32] = {};
    char     ssid[33] = {};
};

struct ApRow {
    bool     used = false;
    char     mac[18] = {};
    char     ssid[33] = {};
    uint8_t  ch = 0, band = 2, sec = 0;
    int8_t   rssi = -100;
    uint32_t last_ms = 0;
};

struct Model {
    Det   dets[MODEL_MAX_DETS];
    int   ndets = 0;
    ApRow aps[MODEL_MAX_APS];
    int   naps = 0;
    uint32_t aps_ms = 0;               // when the AP table was last refreshed
    uint32_t list_ms = 0;              // when a full list sweep last completed
    uint8_t  sweep = 0;
    uint16_t list_total = 0;           // detections the C5 reported in its last list page
};

extern Model g_model;

// A detection map (an item of LIST_DETECTIONS, or an SS_EVT_DETECTION body). Returns the slot
// index or -1. *is_new / *alert report the event flags when present.
int  model_ingest_det(const uint8_t* cbor, size_t n, bool* is_new, bool* alert);
// One LIST_DETECTIONS reply page. Returns the next offset to request, or 0 when the sweep is
// complete (entries the C5 no longer lists are dropped when it completes).
uint16_t model_ingest_list(const uint8_t* cbor, size_t n);
void model_ingest_aps(const uint8_t* cbor, size_t n);

Det* model_find(const char* mac);
int  model_visible_count();                          // detections the UI shows (type != unknown)
int  model_alert_count();                            // visible and at/over the alert threshold
void model_prune(uint32_t now_ms);                   // drop detections the C5 stopped reporting
void model_clear();                                  // e.g. on a new C5 session
