// SPDX-License-Identifier: GPL-3.0-or-later
// model.h — the CYD's view of what the C5 has heard: detections and access points, built from
// LIST_DETECTIONS / SCAN_WIFI replies and SS_EVT_DETECTION events. The C5 stays the source of
// truth; this is a cache the UI draws from.
#pragma once
#include <Arduino.h>

#define MODEL_MAX_DETS 40                // the C5 lists at most 32; the rest is room for pivoted peers / tapped APs
#define MODEL_MAX_APS  24
#define MODEL_MAX_BLE  16

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

struct BleRow {
    bool     used = false;
    char     mac[18] = {};
    char     name[24] = {};
    int8_t   rssi = -100;
    bool     has_company = false;
    uint16_t company = 0;
    uint32_t last_ms = 0;
};

struct Model {
    Det   dets[MODEL_MAX_DETS];
    int   ndets = 0;
    ApRow aps[MODEL_MAX_APS];
    int   naps = 0;
    BleRow ble[MODEL_MAX_BLE];
    int   nble = 0;
    uint16_t ble_total = 0;            // devices the C5 reported (it sends the 20 freshest; we keep 16)
    uint32_t ble_ms = 0;
    uint32_t aps_ms = 0;               // when the AP table was last refreshed
    uint32_t list_ms = 0;              // when a full list sweep last completed
    uint8_t  sweep = 0;
    uint16_t list_total = 0;           // detections the C5 reported in its last list page
};

// Values mirrored from the C5's detection.h (they travel as plain integers on the wire)
enum : uint8_t { AT_UNKNOWN = 0, AT_BLE_PUBLIC, AT_BLE_RANDOM_STATIC, AT_BLE_RPA, AT_BLE_NRPA, AT_WIFI_UNIVERSAL, AT_WIFI_LOCAL };
enum : uint8_t { SEC_UNKNOWN = 0, SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA2_WPA3, SEC_WPA3, SEC_WPA2_ENT, SEC_WPA3_ENT, SEC_OWE };
#define WSTD_N  0x01
#define WSTD_AC 0x02
#define WSTD_AX 0x04
#define WSTD_BE 0x08

// GET_DEVICE_PROFILE reply for ONE detection (the one the INFO page is showing)
struct Profile {
    bool     have = false;
    char     mac[18] = {};
    uint32_t got_ms = 0;
    uint8_t  atype = 0;
    int8_t   rmin = 0, rmax = 0;
    bool     corr = false;
    char     dev_name[24] = {};
    int32_t  company = -1;
    bool     has_txp = false;  int8_t txp = 0;
    bool     has_appear = false; uint16_t appear = 0;
    bool     has_apple = false; uint8_t apple = 0, apple_i = 0;
    uint16_t svc[4] = {};  uint8_t nsvc = 0;
    char     probe[33] = {};
    bool     wifi = false;  uint8_t wsec = 0, wstd = 0;  uint16_t bint = 0;  bool hidden = false;
    char     country[3] = {};
    // Remote ID (positions are scaled integers on the wire: no floats in the CBOR)
    bool     rid = false;
    uint8_t  uat = 0;
    char     uas[24] = {}, op[24] = {};
    bool     loc = false;       int32_t lat_e6 = 0, lon_e6 = 0;
    bool     has_h = false;     int32_t h_dm = 0;
    bool     has_alt = false;   int32_t alt_dm = 0;
    bool     has_spd = false;   int32_t spd_cms = 0;
    bool     has_vs = false;    int32_t vs_cms = 0;
    bool     has_hdg = false;   int32_t hdg = 0;
    bool     op_pos = false;    int32_t olat_e6 = 0, olon_e6 = 0;
};
extern Profile g_profile;
bool model_ingest_profile(const uint8_t* cbor, size_t n);

// Focus view: what the C5's parked radio has collected about the focused target
// (GET_DEVICE_PROFILE {view:"focus"}). Headers only; no payload is ever carried.
#define FV_MAX_PEERS 12
struct FvPeer {
    char     mac[18] = {};
    uint8_t  role = 0;                 // 0 unknown, 1 client of the target, 2 the AP the target uses
    int8_t   rssi = -127;
    uint32_t tof = 0, frf = 0;         // frames target -> peer, peer -> target
    uint32_t tob = 0, frb = 0;         // bytes
    uint32_t qos = 0, age_ms = 0;
};
// Open-network profiling (issue #11): what one device near the focused target announced about itself.
// Only present when the owner switched the feature on; the C5 never reads payloads beyond these fields.
struct FvOnet {
    char     mac[18] = {};
    char     host[24] = {};            // DHCP hostname
    char     vc[20] = {};              // DHCP vendor class
    char     nm[3][20] = {};           // mDNS names it announces
    uint8_t  nnm = 0;
    char     sv[2][20] = {};           // mDNS service types
    uint8_t  nsv = 0;
    char     ss[28] = {};              // SSDP SERVER
    uint32_t tcp = 0, udp = 0, icmp = 0;
    uint16_t pt[4][3] = {};            // {port, proto (6 TCP / 17 UDP), count}
    uint8_t  npt = 0;
};
struct FocusView {
    bool     have = false;
    char     mac[18] = {};
    bool     wifi = false;
    uint8_t  ch = 0;
    uint32_t got_ms = 0, up_ms = 0, age_ms = 0;
    int8_t   rssi = -127;
    uint32_t n = 0, beacons = 0, presp = 0, preq = 0, omgmt = 0, data = 0;
    uint32_t advs = 0, adv_int = 0;
    uint8_t  ht = 0;
    bool     has_iemap = false;
    uint8_t  iemap[32] = {};
    uint8_t  vie[8][4] = {};
    uint8_t  nvie = 0;
    uint8_t  raw[62] = {};
    uint8_t  raw_len = 0;
    FvPeer   peers[FV_MAX_PEERS];
    uint8_t  npeers = 0;
    FvOnet   onet[6];
    uint8_t  nonet = 0;
};
const FvOnet* model_onet_for(const char* mac);   // the announcements for one MAC in the current focus view, or null
extern FocusView g_fv;
bool model_ingest_focus(const uint8_t* cbor, size_t n);
void model_focus_clear();

// RSSI_TICK events for the focused target (HUNT)
struct HuntTick {
    char     mac[18] = {};
    int8_t   rssi = -127;
    uint32_t heard_ms = 0;             // local time the target was last heard
    bool     ever = false;
};
extern HuntTick g_hunt;
extern char g_focus_keep[18];      // MAC the list sync must not drop (the focused peer)
void model_ingest_tick(const uint8_t* cbor, size_t n);

// A peer picked on LINKS / TRAFF becomes a local placeholder until the C5 lists it
Det* model_add_peer(const char* mac, uint8_t radio, uint8_t ch, int rssi);

extern Model g_model;

// A detection map (an item of LIST_DETECTIONS, or an SS_EVT_DETECTION body). Returns the slot
// index or -1. *is_new / *alert report the event flags when present.
int  model_ingest_det(const uint8_t* cbor, size_t n, bool* is_new, bool* alert);
// One LIST_DETECTIONS reply page. Returns the next offset to request, or 0 when the sweep is
// complete (entries the C5 no longer lists are dropped when it completes).
uint16_t model_ingest_list(const uint8_t* cbor, size_t n);
void model_ingest_aps(const uint8_t* cbor, size_t n);
void model_ingest_ble(const uint8_t* cbor, size_t n);   // SCAN_BLE reply; kept strongest-first

Det* model_find(const char* mac);
int  model_visible_count();                          // detections the UI shows (type != unknown)
int  model_alert_count();                            // visible and at/over the alert threshold
void model_prune(uint32_t now_ms);                   // drop detections the C5 stopped reporting
void model_clear();                                  // e.g. on a new C5 session
