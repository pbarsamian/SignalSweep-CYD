// SignalSweep CYD — UI v3.20
// Direct TFT rendering — no sprites (too much RAM on ESP32).
// Touch debug output enabled — watch serial to verify taps.

#include <TFT_eSPI.h>
#include <Arduino.h>
#include "touch.h"
#include "board_pins.h"
#include "detection.h"
#include "settings.h"
#include "vendor.h"
#include "focus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The UI draws from a snapshot copy; changes must go to the live registry.
extern DetectionRegistry g_reg;
extern SemaphoreHandle_t g_reg_mutex;

static void purge_type(DetectionRegistry& r, ThreatType t) {
    for (int j=0; j<r.count; ) {
        if (r.items[j].type == t) {
            for (int k=j; k<r.count-1; k++) {
                r.items[k] = r.items[k+1];
                memcpy(r.history[k], r.history[k+1], RSSI_HISTORY);
            }
            r.count--;
        } else j++;
    }
}

#define HDR_H    16
#define TAB_H    34    // touch-friendly but slightly tighter
#define PAGE_Y   HDR_H
#define PAGE_H   (SCREEN_H - HDR_H - TAB_H)
#define TAB_Y    (SCREEN_H - TAB_H)
#define TAB_W    80
#define ROW_H    25    // compact rows
#define ROWS_VIS (PAGE_H / ROW_H)

// ── Colour palette ────────────────────────────────────────────────────────────
// Standard RGB565 values. Any panel-level colour correction is done in
// config/User_Setup.h (TFT_RGB_ORDER / TFT_INVERSION_*), not here.
#define C_BLACK   TFT_BLACK
#define C_WHITE   TFT_WHITE
#define C_BG      TFT_BLACK
#define C_PANEL   0x1082
#define C_PANEL2  0x0841
#define C_HDR     TFT_BLACK
#define C_BORDER  0x4208
#define C_GRID    0x2104
#define C_LTGREY  TFT_LIGHTGREY
#define C_MDGREY  0x7BCF
#define C_DKGREY  0x4208
#define C_SIGNAL  TFT_GREEN      // 0x07E0
#define C_SIGNAL2 0x0360         // dim green
#define C_ACCENT  TFT_CYAN
#define C_DRONE   TFT_BLUE
#define C_TRACKER TFT_ORANGE
#define C_CAMERA  TFT_RED
#define C_BODYCAM TFT_MAGENTA
#define C_ATTACK  0xFD20         // amber

static const uint16_t TCOL[] = {C_DRONE,C_TRACKER,C_CAMERA,C_BODYCAM,C_ATTACK,C_DKGREY};
static const char* TNAME[] = {"Drone","Tracker","Camera","BodyCam","Attack","?"};
static const char* TSHRT[] = {"DRN","TRK","CAM","BCM","ATK","?"};

enum class Tab { SCOPE=0, LIST=1, DETAIL=2, SETTINGS=3 };

static TFT_eSPI tft;
static Tab      s_tab        = Tab::SCOPE;
static int      s_scroll     = 0;
static uint8_t  s_scope[320] = {};
static bool     s_was_touched    = false;
static Tab      s_last_tab       = (Tab)99;  // force first draw
static int      s_last_alert_cnt = -1;       // track for header redraw
static bool     s_page_dirty     = true;     // full page needs redraw

static bool inRect(int tx,int ty,int x,int y,int w,int h) {
    return tx>=x && tx<x+w && ty>=y && ty<y+h;
}

// ── Header ────────────────────────────────────────────────────────────────────
static void draw_header(int alert_count) {
    tft.fillRect(0, 0, SCREEN_W, HDR_H, C_BLACK);
    // Accent bottom line — cyan
    tft.drawFastHLine(0, HDR_H-1, SCREEN_W, C_ACCENT);
    // Brand
    tft.setTextColor(C_ACCENT, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("SIGNALSWEEP", 4, HDR_H/2, 1);
    // Separator dot
    tft.fillCircle(82, HDR_H/2, 1, C_MDGREY);
    // Band
    tft.setTextColor(C_LTGREY, C_BLACK);
    tft.drawString("2.4G+BLE", 88, HDR_H/2, 1);
    // Alert badge — pill style
    char buf[24];
    snprintf(buf, sizeof(buf), "%d ALERTS", alert_count);
    uint16_t badge_col = alert_count > 0 ? C_CAMERA : C_DKGREY;
    tft.fillRect(SCREEN_W-68, 2, 66, HDR_H-4, badge_col);
    // Pill end caps (rounded feel via circles)
    tft.fillCircle(SCREEN_W-68, HDR_H/2, (HDR_H-4)/2, badge_col);
    tft.fillCircle(SCREEN_W-2,  HDR_H/2, (HDR_H-4)/2, badge_col);
    tft.setTextColor(C_WHITE, badge_col);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(buf, SCREEN_W-35, HDR_H/2, 1);
}

// ── Tab bar ───────────────────────────────────────────────────────────────────
static const char* TAB_NAMES[] = {"SCOPE","LIST","INFO","SET"};

static void draw_tabs() {
    tft.fillRect(0, TAB_Y, SCREEN_W, TAB_H, C_BLACK);
    tft.drawFastHLine(0, TAB_Y, SCREEN_W, C_BORDER);
    for (int i=0;i<4;i++) {
        int x = i*TAB_W;
        bool act = ((int)s_tab == i);
        // Active tab: subtle bg tint + 2px cyan top line
        if (act) {
            tft.fillRect(x, TAB_Y, TAB_W, TAB_H, 0x0082);
            tft.drawFastHLine(x, TAB_Y,   TAB_W, C_ACCENT);
            tft.drawFastHLine(x, TAB_Y+1, TAB_W, 0x0488); // softer second line
        }
        // Vertical separator between tabs
        if (i > 0) tft.drawFastVLine(x, TAB_Y+4, TAB_H-8, C_BORDER);

        tft.setTextColor(act ? C_ACCENT : C_LTGREY, act ? 0x0082 : C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(TAB_NAMES[i], x+TAB_W/2, TAB_Y+TAB_H/2, 1);
    }
}

// ── Scope ─────────────────────────────────────────────────────────────────────
static void draw_scope(const DetectionRegistry& reg) {
    // NO full-page clear here — causes flicker. Draw each region in-place.

    // Layout:
    //   Left margin (LMARGIN): dB axis labels
    //   Right margin (RMARGIN): small gap so ch11 isn't crammed to edge
    //   Canvas: sx to sx+sw-1
    const int LMARGIN = 24;
    const int RMARGIN = 6;
    int sy = PAGE_Y + 1;
    int sh = 90;
    int sx = LMARGIN;
    int sw = SCREEN_W - LMARGIN - RMARGIN;  // 320-24-6 = 290px wide

    // dB axis labels — right-aligned in left margin, white on black
    const char* db_labels[] = {"-40","-55","-70","-85","-100"};
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_WHITE, C_BLACK);
    for (int i=0; i<5; i++) {
        int y = sy + (i * (sh-1)) / 4;
        tft.drawString(db_labels[i], LMARGIN-2, y, 1);
    }

    // Canvas — BLACK fill, border
    tft.fillRect(sx, sy, sw, sh, C_BLACK);
    tft.drawRect(sx, sy, sw, sh, C_BORDER);

    // Horizontal dB grid lines inside canvas
    for (int i=1; i<4; i++) {
        int y = sy + (i * sh) / 4;
        tft.drawFastHLine(sx+1, y, sw-2, C_GRID);
    }

    // Vertical channel guide lines — one per channel, very dim
    for (int ch=1; ch<=11; ch++) {
        int xpix = sx + ((ch-1) * (sw-1)) / 10;
        tft.drawFastVLine(xpix, sy+1, sh-2, C_GRID);
    }

    // GREEN spectrum — bright top half, dim green fill below (v3.3 confirmed working)
    for (int x=0; x<sw; x++) {
        int si = (x * 319) / (sw-1);
        uint8_t v = s_scope[si];
        int h = (v * (sh-4)) / 100;
        if (h > 0) {
            int top_h = h / 2 + 1;
            int bot_h = h - top_h;
            int top_y = sy + sh - 2 - h;
            tft.drawFastVLine(sx+x, top_y, top_h, C_SIGNAL);
            if (bot_h > 0)
                tft.drawFastVLine(sx+x, top_y + top_h, bot_h, C_SIGNAL2);
        }
    }

    // Scan slot indicator — top-right inside canvas (6s slots)
    bool ble_slot = (millis()/6000)%2==0;
    tft.setTextColor(ble_slot ? C_DRONE : C_SIGNAL, C_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(ble_slot ? "BLE" : "WiFi", sx+sw-3, sy+3, 1);

    // X axis — all 11 channels labelled, with tick marks
    // "Ch" label at far left, then 1-11 aligned to their canvas positions
    int xaxis_y = sy + sh + 1;
    tft.setTextColor(C_LTGREY, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Ch", 0, xaxis_y+3, 1);  // "Ch" label left of axis

    tft.setTextDatum(TC_DATUM);
    for (int ch=1; ch<=11; ch++) {
        int xpix = sx + ((ch-1) * (sw-1)) / 10;
        // Tick mark from canvas bottom
        tft.drawFastVLine(xpix, sy+sh, 4, C_LTGREY);
        // Number — every channel
        char cbuf[4]; snprintf(cbuf, 4, "%d", ch);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.drawString(cbuf, xpix, xaxis_y+4, 1);
    }
    // "Channels" label centred under axis
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Channels", sx + sw/2, xaxis_y+13, 1);

    // Detection rows — drawn in-place, no full clear
    int ry = sy + sh + 24;
    int shown = 0;

    for (int i=0; i<reg.count && ry+ROW_H <= PAGE_Y+PAGE_H; i++) {
        const Detection& d = reg.items[i];
        if (d.type==ThreatType::UNKNOWN) continue;
        uint16_t tc = TCOL[(int)d.type];
        uint16_t bg = (shown%2==0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, ry, SCREEN_W, ROW_H-1, bg);
        // Left colour strip — 3px, full row height
        tft.fillRect(0, ry, 3, ROW_H-1, tc);
        // Type tag — small coloured text
        tft.setTextColor(tc, bg);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(TSHRT[(int)d.type], 6, ry+ROW_H/2, 1);
        // Name
        tft.setTextColor(C_WHITE, bg);
        tft.drawString(d.name, 30, ry+ROW_H/2, 1);
        // RSSI — right aligned, lighter
        char rb[12]; snprintf(rb,12,"%d", d.rssi);
        tft.setTextColor(tc, bg);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(rb, SCREEN_W-2, ry+ROW_H/2, 1);
        ry += ROW_H;
        shown++;
    }
    // Clear any leftover rows from previous frame
    if (ry < PAGE_Y+PAGE_H)
        tft.fillRect(0, ry, SCREEN_W, PAGE_Y+PAGE_H-ry, C_BLACK);

    if (shown==0) {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No detections", SCREEN_W/2, sy+sh+40, 2);
    }
}

// ── List ─────────────────────────────────────────────────────────────────────
static void draw_list(const DetectionRegistry& reg) {
    // No full clear — draw rows in-place to avoid flicker

    int total = 0;
    for (int i=0;i<reg.count;i++) if(reg.items[i].type!=ThreatType::UNKNOWN) total++;

    if (total==0) {
        tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK);
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No signals detected", SCREEN_W/2, PAGE_Y+PAGE_H/2, 2);
        return;
    }

    int ry = PAGE_Y;
    int drawn = 0, skipped = 0;
    for (int i=0; i<reg.count; i++) {
        const Detection& d = reg.items[i];
        if (d.type==ThreatType::UNKNOWN) continue;
        if (skipped < s_scroll) { skipped++; continue; }
        if (drawn >= ROWS_VIS) break;

        uint16_t tc = TCOL[(int)d.type];
        uint16_t bg = (drawn%2==0) ? C_PANEL : C_PANEL2;

        tft.fillRect(0, ry, SCREEN_W-14, ROW_H, bg);
        // Left colour strip
        tft.fillRect(0, ry, 3, ROW_H, tc);
        // Type tag
        tft.setTextColor(tc, bg);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(TSHRT[(int)d.type], 6, ry+ROW_H/2, 1);
        // Name
        tft.setTextColor(d.muted ? C_MDGREY : C_WHITE, bg);
        tft.drawString(d.name, 30, ry+ROW_H/2, 1);
        // RSSI bars — 5 segments right of name area
        int bars = d.rssi_bars();
        for (int b=0;b<5;b++) {
            uint16_t bc = (b<bars) ? tc : C_BORDER;
            int bh = 3 + b*2;
            tft.fillRect(234+b*8, ry+ROW_H-2-bh, 6, bh, bc);
        }
        // Age — coloured
        uint32_t age=(millis()-d.timestamp_ms)/1000;
        char tbuf[10];
        if(age<60) snprintf(tbuf,10,"%lus",(unsigned long)age);
        else snprintf(tbuf,10,"%lum",(unsigned long)(age/60));
        tft.setTextColor(C_MDGREY, bg);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(tbuf, 230, ry+ROW_H/2, 1);

        ry += ROW_H;
        drawn++;
    }

    // Clear any rows left over from previous longer list
    if (ry < TAB_Y)
        tft.fillRect(0, ry, SCREEN_W-14, TAB_Y-ry, C_BLACK);

    // Scroll bar strip (rightmost 14px)
    tft.fillRect(SCREEN_W-14, PAGE_Y, 14, PAGE_H, C_HDR);
    tft.drawFastVLine(SCREEN_W-14, PAGE_Y, PAGE_H, C_BORDER);

    // Up arrow
    uint16_t uc = (s_scroll > 0) ? C_ACCENT : C_BORDER;
    tft.fillTriangle(SCREEN_W-7, PAGE_Y+8,
                     SCREEN_W-13, PAGE_Y+20,
                     SCREEN_W-1,  PAGE_Y+20, uc);

    // Down arrow — keep well clear of tab bar
    uint16_t dc = (s_scroll+ROWS_VIS < total) ? C_ACCENT : C_BORDER;
    tft.fillTriangle(SCREEN_W-7,  TAB_Y-22,
                     SCREEN_W-13, TAB_Y-34,
                     SCREEN_W-1,  TAB_Y-34, dc);

    // Scroll thumb
    if (total > ROWS_VIS) {
        int track_h = PAGE_H - 48;
        int thumb_h = max(12, track_h * ROWS_VIS / total);
        int thumb_y = PAGE_Y + 24 + (s_scroll * (track_h - thumb_h)) / max(1, total - ROWS_VIS);
        tft.fillRect(SCREEN_W-12, thumb_y, 10, thumb_h, C_DKGREY);
    }
}

// ── Detail (scrollable, everything we know about one device) ────────────────
// Tracked by MAC, not list position, so pruning can't swap the device shown.
static uint8_t   s_detail_mac[6] = {};
static bool      s_detail_have   = false;
static Detection s_detail_last;          // last copy, shown if it ages out
static bool      s_detail_gone   = false;
static int       s_detail_scroll = 0;
static uint32_t  s_detail_drawn  = 0;
static Settings* g_cfg_ref = nullptr;   // set each ui_update for the hunt path

#define DROW_H   18
#define DLIST_Y  (DHEAD_Y + 2)
#define DLIST_H  (TAB_Y - DLIST_Y - 2)
#define DVIS     (DLIST_H / DROW_H)
#define DVAL_X   92
#define DSTRIP_W 16

struct DRow { const char* k; char v[40]; };
static DRow s_rows[48];
static int  s_nrows = 0;

static char* addrow(const char* k) {
    if (s_nrows >= (int)(sizeof(s_rows)/sizeof(s_rows[0]))) return nullptr;
    s_rows[s_nrows].k = k;
    s_rows[s_nrows].v[0] = 0;
    return s_rows[s_nrows++].v;
}
#define ROW(k, ...) do { char* _v = addrow(k); if (_v) snprintf(_v, 40, __VA_ARGS__); } while (0)

static void fmt_ago(char* out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000;
    if (s < 60)        snprintf(out, n, "%us ago", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um %us ago", (unsigned)(s/60), (unsigned)(s%60));
    else               snprintf(out, n, "%uh %um ago", (unsigned)(s/3600), (unsigned)((s/60)%60));
}

static const char* addr_type_str(uint8_t t) {
    switch (t) {
        case ADDR_BLE_PUBLIC:        return "BLE public (IEEE)";
        case ADDR_BLE_RANDOM_STATIC: return "BLE random static";
        case ADDR_BLE_RPA:           return "BLE private (rotating)";
        case ADDR_BLE_NRPA:          return "BLE random (non-resolvable)";
        case ADDR_WIFI_UNIVERSAL:    return "WiFi universal (IEEE)";
        case ADDR_WIFI_LOCAL:        return "WiFi local / randomized";
        default:                     return "Unknown";
    }
}

static const char* wifi_sec_str(uint8_t s) {
    switch (s) {
        case SEC_OPEN:      return "Open (no encryption)";
        case SEC_WEP:       return "WEP";
        case SEC_WPA:       return "WPA (TKIP)";
        case SEC_WPA2:      return "WPA2-Personal";
        case SEC_WPA2_WPA3: return "WPA2/WPA3 transition";
        case SEC_WPA3:      return "WPA3-Personal (SAE)";
        case SEC_WPA2_ENT:  return "WPA2-Enterprise";
        case SEC_WPA3_ENT:  return "WPA3-Enterprise 192";
        case SEC_OWE:       return "Enhanced Open (OWE)";
        default:            return nullptr;
    }
}

static void build_rows(const Detection& d, uint32_t now) {
    s_nrows = 0;
    char mac[20]; d.mac_str(mac);
    char t[40];

    // ── Identity
    ROW("Type", "%s", TNAME[(int)d.type]);
    ROW("Confidence", "%d%% %s%s", d.confidence,
        d.confidence>=75?"Confirmed":(d.confidence>=45?"Likely":"Possible"),
        d.corroborated?" (+BLE/WiFi)":"");
    ROW("MAC", "%s", mac);
    ROW("Address", "%s", addr_type_str(d.addr_type));
    if (d.addr_type == ADDR_BLE_PUBLIC || d.addr_type == ADDR_WIFI_UNIVERSAL) {
        const char* v = vendor_for_mac(d.mac);
        if (v) ROW("Vendor", "%s", v);
        else   ROW("Vendor", "%s", vendor_db_full() ? "Not in IEEE registry" : "Unknown (run vendor tool)");
    } else {
        ROW("Vendor", "n/a (random address)");
    }
    if (d.company >= 0) {
        const char* c = vendor_for_company((uint16_t)d.company);
        if (c) ROW("BT company", "%s (0x%04X)", c, (unsigned)d.company);
        else   ROW("BT company", "0x%04X", (unsigned)d.company);
    }
    if (d.dev_name[0])   ROW("Device name", "%s", d.dev_name);
    if (d.ssid[0])       ROW("SSID", "%s", d.ssid);
    if (d.hidden_ssid)   ROW("SSID", "(hidden network)");
    if (d.probe_ssid[0]) ROW("Probing for", "%s", d.probe_ssid);
    if (d.apple_type != 0xFF) ROW("Apple", "%s", apple_role(d.apple_type, d.apple_info));
    if (d.has_appearance) {
        const char* ap = ble_appearance(d.appearance);
        if (ap) ROW("Appearance", "%s", ap); else ROW("Appearance", "0x%04X", d.appearance);
    }

    // ── Signal
    ROW("RSSI", "%d dBm  (%d .. %d)", d.rssi, d.rssi_min, d.rssi_max);
    if (d.tx_power != TXP_NONE) {
        ROW("TX power", "%+d dBm", d.tx_power);
        // Log-distance model: RSSI@1m ~ TX - 41 dB (2.4 GHz free space), n = 2.5
        float dist = powf(10.0f, ((d.tx_power - 41) - d.rssi) / 25.0f);
        if (dist < 1.0f) ROW("Est. distance", "< 1 m (rough)");
        else             ROW("Est. distance", "~%.0f m (rough)", dist);
    }
    if (d.channel >= 1 && d.channel <= 14)
        ROW("Channel", "WiFi ch %d (%d MHz)", d.channel, d.channel == 14 ? 2484 : 2407 + 5 * d.channel);
    else
        ROW("Channel", "BLE");
    ROW("Heard via", "%s", d.proto == (PROTO_BLE|PROTO_WIFI) ? "BLE + WiFi" :
                           d.proto == PROTO_WIFI ? "WiFi" : "BLE");

    // ── Timing
    fmt_ago(t, sizeof(t), now - d.first_seen_ms); ROW("First seen", "%s", t);
    fmt_ago(t, sizeof(t), now - d.timestamp_ms);  ROW("Last seen", "%s", t);
    ROW("Times heard", "%u", (unsigned)d.seen_count);

    // ── BLE details
    if (d.proto & PROTO_BLE) {
        ROW("Connectable", "%s", d.connectable ? "Yes" : "No (broadcast only)");
        for (int i = 0; i < d.svc_count; i++) {
            const char* n = name_for_uuid16(d.svc[i]);
            if (n) ROW("Service", "0x%04X %s", d.svc[i], n); else ROW("Service", "0x%04X", d.svc[i]);
        }
        if (d.svc_128) ROW("Service", "+ custom 128-bit UUID");
    }

    // ── WiFi details
    if (d.proto & PROTO_WIFI) {
        const char* fr = d.wifi_frame == WF_BEACON ? "Beacon (access point)" :
                         d.wifi_frame == WF_PROBE_RESP ? "Probe response (AP)" :
                         d.wifi_frame == WF_PROBE_REQ ? "Probe request (client)" :
                         d.wifi_frame == WF_NAN ? "NAN action (Remote ID)" : nullptr;
        if (fr) ROW("Frame", "%s", fr);
        const char* sec = wifi_sec_str(d.wifi_sec);
        if (sec) ROW("Security", "%s", sec);
        if (d.wifi_frame == WF_BEACON || d.wifi_frame == WF_PROBE_RESP || d.wifi_std) {
            const char* st = (d.wifi_std & WSTD_BE) ? "802.11be (WiFi 7)" :
                             (d.wifi_std & WSTD_AX) ? "802.11ax (WiFi 6)" :
                             (d.wifi_std & WSTD_AC) ? "802.11ac (WiFi 5)" :
                             (d.wifi_std & WSTD_N)  ? "802.11n (WiFi 4)" : "802.11b/g";
            ROW("Standard", "%s", st);
        }
        if (d.country[0]) ROW("Country", "%s", d.country);
        if (d.beacon_int) ROW("Beacon int.", "%u TU (%u ms)", d.beacon_int,
                              (unsigned)((d.beacon_int * 1024UL) / 1000));
    }

    // ── Drone (ASTM F3411 Remote ID)
    if (d.has_remote_id) {
        const RemoteID& r = d.remote_id;
        if (r.uas_id[0])      ROW("UAS ID", "%s", r.uas_id);
        if (r.uas_id[0])      ROW("UA type", "%s", odid_ua_type(r.ua_type));
        if (r.operator_id[0]) ROW("Operator ID", "%s", r.operator_id);
        if (r.loc_valid) {
            if (r.lat != 0 || r.lon != 0) ROW("Drone pos", "%.5f, %.5f", r.lat, r.lon);
            if (r.height_agl > -1000) ROW("Height AGL", "%.0f m", r.height_agl);
            if (r.alt_geo > -1000)    ROW("Altitude", "%.0f m (WGS84)", r.alt_geo);
            if (r.speed_ms < 254)     ROW("Speed", "%.1f m/s (%.0f km/h)", r.speed_ms, r.speed_ms * 3.6f);
            if (r.vspeed_ms < 62)     ROW("Vert. speed", "%+.1f m/s", r.vspeed_ms);
            if (r.heading < 360)      ROW("Heading", "%.0f deg", r.heading);
        }
        if (r.op_valid) ROW("Pilot pos", "%.5f, %.5f", r.op_lat, r.op_lon);
    }

    ROW("Muted", "%s", d.muted ? "Yes" : "No");
}

// Fit a value into the space right of the label column
static void draw_fit(const char* v, int x, int y, int maxw) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%s", v);
    int n = strlen(buf);
    while (n > 2 && tft.textWidth(buf, 2) > maxw) {
        buf[--n] = 0;
        buf[n-1] = '.'; buf[n-2] = '.';
    }
    tft.drawString(buf, x, y, 2);
}

enum DetailPage { DP_INFO=0, DP_LINKS=1, DP_TRAFFIC=2, DP_RAW=3, DP_HUNT=4, DP_CAP=5, DP_COUNT=6 };
static const char* DP_NAME[DP_COUNT] = { "INFO", "LINKS", "TRAFF", "RAW", "HUNT", "CAP" };
static DetailPage s_dpage = DP_INFO;

#define SUBTAB_Y (PAGE_Y + 26)
#define SUBTAB_H 18
#define DHEAD_Y  (SUBTAB_Y + SUBTAB_H)

static void draw_subtabs(const Detection& d) {
    uint16_t tc = TCOL[(int)d.type];
    int w = SCREEN_W / DP_COUNT;
    for (int i = 0; i < DP_COUNT; i++) {
        bool on = (i == (int)s_dpage);
        int x = i * w;
        tft.fillRect(x, SUBTAB_Y, w-1, SUBTAB_H-1, on ? tc : C_PANEL);
        tft.setTextColor(on ? C_BLACK : C_MDGREY, on ? tc : C_PANEL);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(DP_NAME[i], x + w/2, SUBTAB_Y + SUBTAB_H/2 - 1, 1);
    }
}

// ── Hunt mode ───────────────────────────────────────────────────────────────
// Lock onto the focused device and climb toward it: click rate and screen/LED
// pulse rise with RSSI. Goes quiet after HUNT_SILENCE_MS of no contact.
#define HUNT_SILENCE_MS 8000
#define HUNT_RSSI_FLOOR -95
#define HUNT_RSSI_CEIL  -35

static bool     s_hunt_on = false;
static uint32_t s_hunt_last_click = 0;
static uint32_t s_hunt_last_pulse = 0;
static bool     s_hunt_pulse_state = false;
static int      s_hunt_peak = -128;
static int      s_hunt_prev_rssi = -128;

// RSSI -> click/pulse interval. Closer = faster. -95 dBm ~1.2 s, -35 dBm ~70 ms.
static uint32_t hunt_interval(int rssi) {
    int r = constrain(rssi, HUNT_RSSI_FLOOR, HUNT_RSSI_CEIL);
    float t = (float)(r - HUNT_RSSI_FLOOR) / (HUNT_RSSI_CEIL - HUNT_RSSI_FLOOR); // 0..1
    return (uint32_t)(1200 - t * 1130);   // 1200ms far -> 70ms near
}

// Heat colour blue -> cyan -> amber -> red as the target gets closer
static uint16_t hunt_heat(int rssi) {
    int r = constrain(rssi, HUNT_RSSI_FLOOR, HUNT_RSSI_CEIL);
    float t = (float)(r - HUNT_RSSI_FLOOR) / (HUNT_RSSI_CEIL - HUNT_RSSI_FLOOR);
    uint8_t R, G, B;
    if (t < 0.5f) { float u=t/0.5f; R=0; G=(uint8_t)(128+127*u); B=(uint8_t)(255-100*u); }
    else          { float u=(t-0.5f)/0.5f; R=(uint8_t)(255*u); G=(uint8_t)(255-100*u); B=(uint8_t)(155-155*u); }
    return tft.color565(R, G, B);
}

static void hunt_led(uint16_t on, int rssi) {
    // Approximate the heat colour on the RGB LED (active LOW)
    int r = constrain(rssi, HUNT_RSSI_FLOOR, HUNT_RSSI_CEIL);
    float t = (float)(r - HUNT_RSSI_FLOOR) / (HUNT_RSSI_CEIL - HUNT_RSSI_FLOOR);
    bool lr = on && t > 0.4f;     // red grows with closeness
    bool lg = on && t < 0.6f;     // green fades with closeness
    bool lb = on && t < 0.3f;     // blue only when far
    digitalWrite(PIN_LED_R, lr?LOW:HIGH);
    digitalWrite(PIN_LED_G, lg?LOW:HIGH);
    digitalWrite(PIN_LED_B, lb?LOW:HIGH);
}

static void hunt_all_off() {
    ledcWrite(1, 0);
    digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);
}

// Called every UI frame while the HUNT sub-tab is active
static void hunt_tick(const Detection& d, const Settings& cfg, bool gone) {
    uint32_t now = millis();
    uint32_t age = now - d.timestamp_ms;
    bool live = !gone && age < HUNT_SILENCE_MS;
    int rssi = live ? d.rssi : -128;
    if (live && rssi > s_hunt_peak) s_hunt_peak = rssi;

    if (live) {
        uint32_t iv = hunt_interval(rssi);
        // Click: short tone burst
        if (cfg.speaker_en && now - s_hunt_last_click >= iv) {
            s_hunt_last_click = now;
            ledcWriteTone(1, 3000);
            // brief — turned off on the next frame via the pulse timer
        } else if (now - s_hunt_last_click >= 12) {
            ledcWrite(1, 0);   // end the click quickly
        }
        // Pulse: screen flash + LED, same cadence as clicks
        if (now - s_hunt_last_pulse >= iv) {
            s_hunt_last_pulse = now;
            s_hunt_pulse_state = !s_hunt_pulse_state;
            if (cfg.led_en) hunt_led(s_hunt_pulse_state, rssi);
        }
    } else {
        hunt_all_off();
        s_hunt_pulse_state = false;
    }
}

static void draw_hunt(const Detection& d, const Settings& cfg, bool gone, bool full) {
    uint32_t now = millis();
    uint32_t age = now - d.timestamp_ms;
    bool live = !gone && age < HUNT_SILENCE_MS;
    int rssi = live ? d.rssi : -128;
    uint16_t heat = live ? hunt_heat(rssi) : C_PANEL;

    int cy = (DHEAD_Y + TAB_Y) / 2;

    // Pulsing ring: filled when the pulse is high, so it strobes with the clicks
    int ringR = 46;
    uint16_t ringc = (live && s_hunt_pulse_state) ? heat : C_BLACK;
    tft.fillCircle(SCREEN_W/2, cy, ringR, ringc);
    tft.drawCircle(SCREEN_W/2, cy, ringR, live ? heat : C_BORDER);

    // Big RSSI number in the middle
    char v[12];
    if (live) snprintf(v, sizeof(v), "%d", rssi); else snprintf(v, sizeof(v), "--");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(live ? C_BLACK : C_MDGREY, ringc);
    tft.drawString(v, SCREEN_W/2, cy-6, 6);
    tft.setTextColor(live ? C_BLACK : C_MDGREY, ringc);
    tft.drawString("dBm", SCREEN_W/2, cy+20, 2);

    // Left column: trend arrow + peak
    tft.setTextDatum(ML_DATUM);
    tft.fillRect(4, DHEAD_Y+6, 70, 60, C_BLACK);
    const char* arrow = "--";
    uint16_t ac = C_MDGREY;
    if (live) {
        if (rssi > s_hunt_prev_rssi + 1) { arrow = "WARMER"; ac = C_TRACKER; }
        else if (rssi < s_hunt_prev_rssi - 1) { arrow = "colder"; ac = C_ACCENT; }
        else { arrow = "steady"; ac = C_LTGREY; }
        s_hunt_prev_rssi = rssi;
    }
    tft.setTextColor(ac, C_BLACK);
    tft.drawString(arrow, 6, DHEAD_Y+16, 2);
    char pk[16]; snprintf(pk, sizeof(pk), "peak %d", s_hunt_peak > -128 ? s_hunt_peak : 0);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.drawString(pk, 6, DHEAD_Y+36, 2);

    // Right column: seconds since last heard
    tft.setTextDatum(MR_DATUM);
    tft.fillRect(SCREEN_W-78, DHEAD_Y+6, 74, 60, C_BLACK);
    char ls[18];
    if (gone) snprintf(ls, sizeof(ls), "aged out");
    else snprintf(ls, sizeof(ls), "%us ago", (unsigned)(age/1000));
    tft.setTextColor(live ? C_LTGREY : C_CAMERA, C_BLACK);
    tft.drawString(ls, SCREEN_W-6, DHEAD_Y+16, 2);
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.drawString(cfg.speaker_en?"sound on":"muted", SCREEN_W-6, DHEAD_Y+36, 2);
}

// Append WPS / chipset / timing identity gathered by focus mode
static void add_focus_rows(const Detection& d) {
    FocusProfile f;
    if (!focus_copy(f) || !f.active || memcmp(f.mac, d.mac, 6) != 0) {
        ROW("Focus", "engaging radio...");
        return;
    }
    if (f.radio == FOCUS_WIFI) {
        if (f.has_wps) {
            if (f.wps_manuf[0])     ROW("WPS maker", "%s", f.wps_manuf);
            if (f.wps_model[0])     ROW("WPS model", "%s", f.wps_model);
            if (f.wps_model_num[0]) ROW("WPS model #", "%s", f.wps_model_num);
            if (f.wps_devname[0])   ROW("WPS name", "%s", f.wps_devname);
            if (f.wps_serial[0])    ROW("WPS serial", "%s", f.wps_serial);
            if (f.wps_state)        ROW("WPS state", "%s", f.wps_state==2?"configured":"unconfigured");
        }
        if (f.ht_streams)   ROW("Spatial streams", "%dx%d", f.ht_streams, f.ht_streams);
        if (f.vie_count) {
            // Name a couple of well-known vendor-IE chipset OUIs
            for (int i = 0; i < f.vie_count; i++) {
                const uint8_t* o = f.vie[i];
                const char* who = nullptr;
                if (o[0]==0x00&&o[1]==0x50&&o[2]==0xF2) who = "Microsoft/WPA";
                else if (o[0]==0x00&&o[1]==0x17&&o[2]==0xF2) who = "Apple";
                else if (o[0]==0x00&&o[1]==0x10&&o[2]==0x18) who = "Broadcom";
                else if (o[0]==0x00&&o[1]==0x03&&o[2]==0x7F) who = "Atheros/Qualcomm";
                else if (o[0]==0x50&&o[1]==0x6F&&o[2]==0x9A) who = "Wi-Fi Alliance";
                if (who) { ROW("Vendor IE", "%s", who); }
            }
        }
        ROW("Frames (focus)", "%u b=%u pr=%u", (unsigned)f.frames,
            (unsigned)f.beacons, (unsigned)f.probe_resps);
    } else {
        ROW("Adverts (focus)", "%u", (unsigned)f.adv_count);
        if (f.adv_int_min) ROW("Advert interval", "~%u ms", (unsigned)f.adv_int_min);
    }
}

// ── RAW page: decoded + hex of the latest advertisement / IE map ────────────
static void draw_raw(const Detection& d, bool full) {
    if (full) tft.fillRect(0, DHEAD_Y, SCREEN_W, TAB_Y - DHEAD_Y, C_BLACK);
    FocusProfile f;
    bool have = focus_copy(f) && f.active && memcmp(f.mac, d.mac, 6) == 0;
    tft.setTextDatum(ML_DATUM);
    int y = DHEAD_Y + 6;
    char line[48];

    if (!have) {
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.drawString("Collecting from target...", 6, y, 2);
        return;
    }
    if (f.radio == FOCUS_BLE) {
        tft.setTextColor(C_ACCENT, C_BLACK);
        tft.drawString("BLE advertising payload", 6, y, 1); y += 14;
        tft.setTextColor(C_WHITE, C_BLACK);
        for (int i = 0; i < f.raw_len && y < TAB_Y - 10; i += 8) {
            char* p = line; p += sprintf(p, "%02X:", i);
            for (int j = i; j < i+8 && j < f.raw_len; j++) p += sprintf(p, " %02X", f.raw[j]);
            tft.fillRect(0, y-1, SCREEN_W, 13, C_BLACK);
            tft.drawString(line, 6, y, 1); y += 13;
        }
    } else {
        tft.setTextColor(C_ACCENT, C_BLACK);
        tft.drawString("802.11 elements present", 6, y, 1); y += 14;
        tft.setTextColor(C_WHITE, C_BLACK);
        // Decode the element-ID bitmap into names
        struct { uint8_t id; const char* n; } K[] = {
            {0,"SSID"},{1,"Rates"},{3,"DS/chan"},{5,"TIM"},{7,"Country"},{11,"BSS load"},
            {45,"HT caps"},{48,"RSN/WPA2"},{50,"Ext rates"},{61,"HT info"},{127,"Ext cap"},
            {191,"VHT caps"},{221,"Vendor"},{255,"Ext (HE/EHT)"} };
        char buf[48]={0}; int bl=0;
        for (auto& k : K)
            if (f.ie_map[k.id>>3] & (1<<(k.id&7)))
                bl += snprintf(buf+bl, sizeof(buf)-bl, "%s ", k.n);
        // wrap at ~38 chars
        char* s2 = buf;
        while (*s2 && y < TAB_Y - 10) {
            char seg[40]; int n=0;
            while (s2[n] && n < 38) n++;
            while (n > 0 && s2[n] != ' ' && s2[n] != 0) n--;
            if (n == 0) n = 38;
            memcpy(seg, s2, n); seg[n]=0;
            tft.fillRect(0, y-1, SCREEN_W, 13, C_BLACK);
            tft.drawString(seg, 6, y, 1); y += 13; s2 += n; while(*s2==' ')s2++;
        }
        y += 4;
        tft.fillRect(0, y-1, SCREEN_W, 13, C_BLACK);
        snprintf(line, sizeof(line), "Vendor IEs: %d   Frames: %u", f.vie_count, (unsigned)f.frames);
        tft.setTextColor(C_MDGREY, C_BLACK);
        tft.drawString(line, 6, y, 1);
    }
}

static void draw_detail(const DetectionRegistry& reg, bool full) {
    // Locate the device by MAC
    const Detection* dp = nullptr;
    if (s_detail_have) {
        for (int i = 0; i < reg.count; i++)
            if (memcmp(reg.items[i].mac, s_detail_mac, 6) == 0) { dp = &reg.items[i]; break; }
    }
    if (!s_detail_have) { s_tab = Tab::LIST; s_page_dirty = true; return; }
    bool gone = (dp == nullptr);
    if (dp) s_detail_last = *dp;
    const Detection& d = s_detail_last;
    if (gone != s_detail_gone) { s_detail_gone = gone; full = true; }
    uint16_t tc = TCOL[(int)d.type];

    if (full) {
        tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK);
        tft.setTextColor(C_ACCENT, C_BLACK);
        tft.setTextDatum(ML_DATUM);
        tft.drawString("< Back", 4, PAGE_Y+10, 2);
        if (!gone) {
            tft.fillRect(SCREEN_W-72, PAGE_Y+2, 70, 22, C_PANEL);
            tft.drawRect(SCREEN_W-72, PAGE_Y+2, 70, 22, C_CAMERA);
            tft.setTextColor(C_CAMERA, C_PANEL);
            tft.setTextDatum(MC_DATUM);
            tft.drawString(d.muted?"Unmute":"Mute", SCREEN_W-37, PAGE_Y+13, 1);
        } else {
            tft.setTextColor(C_MDGREY, C_BLACK);
            tft.setTextDatum(MR_DATUM);
            tft.drawString("aged out - last data", SCREEN_W-4, PAGE_Y+10, 1);
        }
        tft.setTextColor(gone ? C_MDGREY : tc, C_BLACK);
        tft.setTextDatum(MC_DATUM);
        // Name centered between the Back button and the Mute button
        char nm[26]; snprintf(nm, sizeof(nm), "%s", d.name);
        int n=strlen(nm);
        while (n>2 && tft.textWidth(nm,1) > SCREEN_W-150) { nm[--n]=0; nm[n-1]='.'; nm[n-2]='.'; }
        tft.drawString(nm, SCREEN_W/2, PAGE_Y+12, 1);
    }

    if (full) draw_subtabs(d);

    // Pages that are not the scrolling INFO list draw themselves and return
    if (s_dpage == DP_RAW)  { draw_raw(d, full);  s_detail_drawn = millis(); return; }
    if (s_dpage == DP_HUNT) {
        if (full) tft.fillRect(0, DHEAD_Y, SCREEN_W, TAB_Y - DHEAD_Y, C_BLACK);
        hunt_tick(d, g_cfg_ref ? *g_cfg_ref : Settings(), gone);
        draw_hunt(d, g_cfg_ref ? *g_cfg_ref : Settings(), gone, full);
        s_detail_drawn = millis();
        return;
    }
    if (s_dpage != DP_INFO) {
        if (full) {
            tft.fillRect(0, DHEAD_Y, SCREEN_W, TAB_Y - DHEAD_Y, C_BLACK);
            tft.setTextColor(C_MDGREY, C_BLACK); tft.setTextDatum(MC_DATUM);
            tft.drawString("(coming in a later version)", SCREEN_W/2, (DHEAD_Y+TAB_Y)/2, 2);
        }
        s_detail_drawn = millis();
        return;
    }

    build_rows(d, gone ? d.timestamp_ms : millis());
    if (!gone) add_focus_rows(d);
    int maxs = max(0, s_nrows - DVIS);
    if (s_detail_scroll > maxs) s_detail_scroll = maxs;

    // Rows: drawn in place every refresh (no page clear, so no flicker)
    int lw = SCREEN_W - DSTRIP_W;
    for (int r = 0; r < DVIS; r++) {
        int y = DLIST_Y + r * DROW_H;
        int i = s_detail_scroll + r;
        uint16_t bg = (i % 2 == 0) ? C_PANEL : C_PANEL2;
        tft.fillRect(0, y, lw, DROW_H - 1, i < s_nrows ? bg : C_BLACK);
        if (i >= s_nrows) continue;
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_MDGREY, bg);
        tft.drawString(s_rows[i].k, 4, y + DROW_H/2, 1);
        tft.setTextColor(gone ? C_LTGREY : C_WHITE, bg);
        draw_fit(s_rows[i].v, DVAL_X, y + DROW_H/2, lw - DVAL_X - 2);
    }

    // Scroll strip: tap top half = up, bottom half = down
    int sx = SCREEN_W - DSTRIP_W;
    tft.fillRect(sx, DLIST_Y, DSTRIP_W, DLIST_H, C_BLACK);
    uint16_t uc = s_detail_scroll > 0 ? C_ACCENT : C_BORDER;
    uint16_t dc = s_detail_scroll < maxs ? C_ACCENT : C_BORDER;
    int cx = sx + DSTRIP_W/2;
    tft.fillTriangle(cx, DLIST_Y+2, cx-6, DLIST_Y+12, cx+6, DLIST_Y+12, uc);
    tft.fillTriangle(cx, DLIST_Y+DLIST_H-2, cx-6, DLIST_Y+DLIST_H-12, cx+6, DLIST_Y+DLIST_H-12, dc);
    if (maxs > 0) {
        int track_y = DLIST_Y + 16, track_h = DLIST_H - 32;
        int th = max(10, track_h * DVIS / s_nrows);
        int ty = track_y + (s_detail_scroll * (track_h - th)) / maxs;
        tft.fillRect(cx-2, track_y, 4, track_h, C_GRID);
        tft.fillRect(cx-2, ty, 4, th, C_ACCENT);
    }
    s_detail_drawn = millis();
}

// ── Settings ──────────────────────────────────────────────────────────────────
static void draw_settings(Settings& s) {
    tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK);
    tft.setTextColor(C_ACCENT, C_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("DETECTION & ALERTS", 4, PAGE_Y+4, 1);
    tft.drawFastHLine(0, PAGE_Y+13, SCREEN_W, C_ACCENT);

    struct { const char* l; const char* sub; bool* v; } rows[] = {
        {"Drones",   "Remote ID", &s.detect_drone},
        {"Trackers", "AirTag/Tile",&s.detect_tracker},
        {"Cameras",  "Flock ALPR",&s.detect_camera},
        {"BodyCams", "Axon",      &s.detect_bodycam},
        {"Attack",   "Flipper",   &s.detect_attack},
        {"Speaker",  "",          &s.speaker_en},
        {"LED",      "",          &s.led_en},
    };
    for (int i=0;i<7;i++) {
        int col=(i<4)?0:1;
        int row=(i<4)?i:(i-4);
        int x=col*162+2;
        int y=PAGE_Y+18+row*42;
        tft.fillRect(x,y,158,38,C_PANEL);
        tft.drawRect(x,y,158,38,C_BORDER);
        tft.setTextColor(C_WHITE, C_PANEL); tft.setTextDatum(ML_DATUM);
        tft.drawString(rows[i].l, x+4, y+6, 2);
        if(rows[i].sub[0]){ tft.setTextColor(C_MDGREY, C_PANEL); tft.drawString(rows[i].sub, x+4, y+24, 1); }
        // Toggle pill
        uint16_t ton = *rows[i].v ? C_SIGNAL : C_DKGREY;
        tft.fillRoundRect(x+122, y+10, 30, 16, 8, ton);
        tft.fillCircle(*rows[i].v ? x+142 : x+127, y+18, 5, C_WHITE);
    }
    tft.setTextColor(C_MDGREY, C_BLACK);
    tft.drawString("SignalSweep v3.20  GPL-3.0", 4, PAGE_Y+PAGE_H-12, 1);
}

// ── Touch ─────────────────────────────────────────────────────────────────────
static void handle_touch(int tx, int ty, const DetectionRegistry& reg, Settings& s) {
    Serial.printf("[tap] tx=%d ty=%d tab=%d TAB_Y=%d\n", tx, ty, (int)s_tab, TAB_Y);

    if (ty >= TAB_Y) {
        int t = tx / TAB_W;
        if (t>=0 && t<4) {
            if (s_tab==Tab::DETAIL && (Tab)t!=Tab::DETAIL) { hunt_all_off(); focus_stop(); }
            Serial.printf("[tap] switching to tab %d\n", t);
            s_tab = (Tab)t;
            s_scroll = 0;
            tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK);
            draw_tabs();
        }
        return;  // never fall through to page handlers on a tab tap
    } else if (s_tab==Tab::LIST) {
        int total=0;
        for(int i=0;i<reg.count;i++) if(reg.items[i].type!=ThreatType::UNKNOWN) total++;

        if (tx >= SCREEN_W-16) {
            int mid = PAGE_Y + (PAGE_H - 36) / 2;  // midpoint above tab bar
            if (ty < mid && s_scroll > 0) s_scroll--;
            else if (ty >= mid && ty < TAB_Y-10 && s_scroll+ROWS_VIS < total) s_scroll++;
            return;
        }
        int row = (ty - PAGE_Y) / ROW_H;
        int seen=0; int ridx=-1;
        for(int i=0;i<reg.count;i++){
            if(reg.items[i].type==ThreatType::UNKNOWN) continue;
            if(seen<s_scroll){seen++;continue;}
            if(seen-s_scroll==row){ridx=i;break;}
            seen++;
        }
        if(ridx>=0){
            const Detection& sel = reg.items[ridx];
            memcpy(s_detail_mac, sel.mac, 6);
            s_detail_have = true; s_detail_gone = false; s_detail_scroll = 0;
            s_dpage = DP_INFO;
            // Stop sweeping, work this one target
            if (sel.proto & PROTO_WIFI)
                focus_start(sel.mac, FOCUS_WIFI, sel.channel >= 1 && sel.channel <= 13 ? sel.channel : 0);
            else
                focus_start(sel.mac, FOCUS_BLE, 0);
            s_tab=Tab::DETAIL; s_page_dirty=true;
        }
    } else if (s_tab==Tab::DETAIL) {
        if (inRect(tx,ty, 0,PAGE_Y, 80,22)) {
            hunt_all_off();
            focus_stop();                 // resume sweeping
            s_tab=Tab::LIST;
            s_page_dirty=true;
        } else if (ty >= SUBTAB_Y && ty < SUBTAB_Y + SUBTAB_H) {
            int w = SCREEN_W / DP_COUNT;
            int p = tx / w;
            if (p >= 0 && p < DP_COUNT && p != (int)s_dpage) {
                if (s_dpage == DP_HUNT) hunt_all_off();   // leaving hunt
                s_dpage = (DetailPage)p;
                s_detail_scroll = 0;
                if (s_dpage == DP_HUNT) {                 // entering hunt
                    s_hunt_peak = -128; s_hunt_prev_rssi = -128;
                    s_hunt_last_click = s_hunt_last_pulse = 0;
                }
                s_page_dirty = true;
            }
        } else if (inRect(tx,ty, SCREEN_W-72,PAGE_Y, 70,22) && !s_detail_gone) {
            bool nm = !s_detail_last.muted;
            if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                for (int j=0; j<g_reg.count; j++)
                    if (memcmp(g_reg.items[j].mac, s_detail_mac, 6) == 0)
                        g_reg.items[j].muted = nm;   // live registry
                xSemaphoreGive(g_reg_mutex);
            }
            for (int j=0; j<reg.count; j++)          // snapshot, for this redraw
                if (memcmp(reg.items[j].mac, s_detail_mac, 6) == 0)
                    const_cast<Detection&>(reg.items[j]).muted = nm;
            s_page_dirty=true;
        } else if (s_dpage==DP_INFO && tx >= SCREEN_W-DSTRIP_W && ty >= DLIST_Y && ty < DLIST_Y+DLIST_H) {
            int page = max(1, DVIS - 1);
            if (ty < DLIST_Y + DLIST_H/2) s_detail_scroll = max(0, s_detail_scroll - page);
            else                          s_detail_scroll += page;   // clamped in draw
            s_page_dirty=true;
        }
    } else if (s_tab==Tab::SETTINGS) {
        bool* vals[]={&s.detect_drone,&s.detect_tracker,&s.detect_camera,
                      &s.detect_bodycam,&s.detect_attack,&s.speaker_en,&s.led_en};
        for(int i=0;i<7;i++){
            int col=(i<4)?0:1; int row=(i<4)?i:(i-4);
            int x=col*162+2+122; int y=PAGE_Y+18+row*42+10;
            if(inRect(tx,ty,x,y,30,16)){
                *vals[i]^=1;
                settings_save(s);
                // If a detection type was just disabled, purge it from registry immediately
                if (i < 5 && !*vals[i]) {
                    purge_type(const_cast<DetectionRegistry&>(reg), (ThreatType)i);
                    if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        purge_type(g_reg, (ThreatType)i);
                        xSemaphoreGive(g_reg_mutex);
                    }
                }
                s_page_dirty=true;
                break;
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void ui_init(Settings& s) {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_TFT_BL, 0);
    ledcWrite(0, s.brightness);

    // Speaker on LEDC channel 1 (SC8002B amp via PIN_SPEAKER) — needed for
    // both alert beeps and hunt-mode clicks.
    ledcSetup(1, 2000, 8);
    ledcAttachPin(PIN_SPEAKER, 1);
    ledcWrite(1, 0);

    touch_init();

    tft.init();
    tft.setRotation(3);
    // Panel colour fix — confirmed by boot colour test (MODE 2):
    // rotation-3 MADCTL with BGR bit (0xE8) + display inversion ON.
    tft.writecommand(0x36);
    tft.writedata(0xE8);
    tft.invertDisplay(true);



    Serial.printf("[ui] init w=%d h=%d PAGE_H=%d ROWS_VIS=%d TAB_Y=%d\n",
                  tft.width(), tft.height(), PAGE_H, ROWS_VIS, TAB_Y);

    draw_header(0);
    draw_tabs();
    tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, C_BLACK);
    tft.fillRect(24, PAGE_Y+1, SCREEN_W-30, 90, C_BLACK);
    tft.drawRect(24, PAGE_Y+1, SCREEN_W-30, 90, C_BORDER);
    tft.setTextColor(C_SIGNAL, C_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Starting up...", SCREEN_W/2, PAGE_Y+47, 2);
}

void ui_update(const DetectionRegistry& reg, Settings& s) {
    g_cfg_ref = &s;
    int tx, ty;
    bool touched = touch_get(tx, ty);
    if (touched && !s_was_touched) {
        handle_touch(tx, ty, reg, s);
        s_page_dirty = true;
    }
    s_was_touched = touched;

    // Header — only redraw when alert count changes
    if (reg.count != s_last_alert_cnt) {
        draw_header(reg.count);
        s_last_alert_cnt = reg.count;
    }

    // Tab bar — only redraw when tab changes
    if (s_tab != s_last_tab) {
        draw_tabs();
        tft.fillRect(0, PAGE_Y, SCREEN_W, PAGE_H, 0x0000); // hard black clear
        s_last_tab = s_tab;
        s_page_dirty = true;
    }

    switch(s_tab) {
        case Tab::SCOPE:
            // Scope always redraws (animated spectrum) — but draws in-place, no full clear
            draw_scope(reg);
            break;
        case Tab::LIST:
            // List redraws when dirty or when detection count changes
            if (s_page_dirty || reg.count != s_last_alert_cnt) {
                draw_list(reg);
                s_page_dirty = false;
            }
            break;
        case Tab::DETAIL:
            if (s_page_dirty) {
                draw_detail(reg, true);
                s_page_dirty = false;
            } else {
                uint32_t iv = (s_dpage == DP_HUNT) ? 30 : 1000;  // hunt updates fast
                if (millis() - s_detail_drawn >= iv) draw_detail(reg, false);
            }
            break;
        case Tab::SETTINGS:
            if (s_page_dirty) {
                draw_settings(s);
                s_page_dirty = false;
            }
            break;
    }
}

void ui_update_scope(const uint8_t* buf, int len) {
    int n = len>320?320:len;
    memcpy(s_scope, buf, n);
}

void ui_alert(const Detection& d, const Settings& s) {
    if (s.speaker_en) {
        uint32_t freq=880;
        switch(d.type){
            case ThreatType::DRONE:   freq=TONE_DRONE;   break;
            case ThreatType::TRACKER: freq=TONE_TRACKER; break;
            case ThreatType::CAMERA:  freq=TONE_CAMERA;  break;
            case ThreatType::BODYCAM: freq=TONE_BODYCAM; break;
            case ThreatType::ATTACK:  freq=TONE_ATTACK;  break;
            default: return;
        }
        ledcWriteTone(1, freq);
        vTaskDelay(pdMS_TO_TICKS(TONE_MS));
        ledcWriteTone(1, 0);
    }
    if (s.led_en) {
        uint8_t r=HIGH,g=HIGH,b=HIGH;
        switch(d.type){
            case ThreatType::DRONE:   b=LOW; break;
            case ThreatType::TRACKER: r=LOW; break;
            case ThreatType::CAMERA:  r=LOW; break;
            case ThreatType::BODYCAM: r=LOW; b=LOW; break;
            case ThreatType::ATTACK:  r=LOW; g=LOW; break;
            default: return;
        }
        digitalWrite(PIN_LED_R,r); digitalWrite(PIN_LED_G,g); digitalWrite(PIN_LED_B,b);
        vTaskDelay(pdMS_TO_TICKS(200));
        digitalWrite(PIN_LED_R,HIGH); digitalWrite(PIN_LED_G,HIGH); digitalWrite(PIN_LED_B,HIGH);
    }
}
