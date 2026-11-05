// SPDX-License-Identifier: GPL-3.0-or-later
// mcp.cpp — read-only MCP server over HTTP (issue #25). See mcp.h.
//
// Why not the core's WebServer: it reads the whole POST body into heap before any handler (or any auth check) runs, with
// no size limit, so one unauthenticated request with a huge Content-Length could exhaust the CYD's memory. This reads the
// request line and headers itself, checks the bearer token, and only then reads at most MCP_MAX_BODY bytes.
//
// The server runs in loop() on the same core as the UI and the model, so the tools read g_model without locking. A
// request is served start to finish before loop() continues; the I/O deadlines are short so a stalled client costs the
// UI under a second.
#include "mcp.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "link.h"
#include "mode.h"
#include "model.h"
#include "listexport.h"
#include "theme.h"                          // CONF_ALERT_MIN
#include "ui.h"
#include "ss_capabilities.h"

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

#define MCP_PORT      80
#define MCP_MAX_BODY  2048
#define MCP_IO_MS     800                  // each request has this long to arrive in full
#define LOCK_TRIES    5                    // bad tokens in a row ...
#define LOCK_MS       30000                // ... lock everyone out for this long

static WiFiServer       s_srv(MCP_PORT);
static bool             s_up = false;
static const C5Status*  s_c5 = nullptr;
static uint8_t          s_bad = 0;
static uint32_t         s_lock_until = 0;
static bool             s_locked = false;
static char             s_body[MCP_MAX_BODY + 1];
// A tool that needs the C5 to poll something the screens are not showing asks for it; main.cpp polls while the want lasts.
#define WANT_MS 20000
static uint32_t         s_aps_until = 0, s_prof_until = 0;
static char             s_prof_mac[18] = {};

void mcp_init(const C5Status* c5) { s_c5 = c5; }
bool mcp_running(void) { return s_up; }
bool mcp_wants_aps(void) { return s_up && (int32_t)(s_aps_until - millis()) > 0; }
bool mcp_wants_profile(char* mac_out) {
    if (!s_up || !s_prof_mac[0] || (int32_t)(s_prof_until - millis()) <= 0) return false;
    strncpy(mac_out, s_prof_mac, 18);
    mac_out[17] = 0;
    return true;
}

// ---- output ----------------------------------------------------------------------------------------------------------
// Buffers small writes so a response goes out in a few TCP segments instead of one per print().
class OutBuf : public Print {
public:
    explicit OutBuf(WiFiClient& c) : c_(c) {}
    size_t write(uint8_t b) override { if (n_ == sizeof(buf_)) flush(); buf_[n_++] = b; return 1; }
    size_t write(const uint8_t* p, size_t len) override { for (size_t i = 0; i < len; i++) write(p[i]); return len; }
    // WiFiClient::write() may accept only part of a buffer; the rest is sent on the next try, for up to 2 s. A reply that
    // could not be sent in full is logged, because the client would otherwise see it silently cut short.
    void flush() override {
        if (!n_) return;
        size_t off = 0;
        uint32_t until = millis() + 2000;
        while (!failed_ && off < n_ && (int32_t)(until - millis()) > 0) {
            size_t w = c_.write(buf_ + off, n_ - off);
            if (w) off += w;
            else if (!c_.connected()) break;
            else delay(2);
        }
        if (off < n_ && !failed_) { failed_ = true; Serial.printf("[mcp] reply cut short: %u of %u bytes sent\n", (unsigned)off, (unsigned)n_); }
        n_ = 0;
    }
private:
    WiFiClient& c_;
    uint8_t buf_[512];
    size_t n_ = 0;
    bool failed_ = false;
};

// Escapes what is written through it so that a JSON document can sit inside a JSON string (the MCP tool result text).
class EscOut : public Print {
public:
    explicit EscOut(Print& o) : o_(o) {}
    size_t write(uint8_t b) override {
        if (b == '"' || b == '\\') { o_.write('\\'); o_.write(b); }
        else if (b < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", b); o_.print(t); }
        else o_.write(b);
        return 1;
    }
private:
    Print& o_;
};

// A JSON string literal, quotes included.
static void jprint(Print& o, const char* s) {
    o.write('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o.write('\\'); o.write(c); }
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); o.print(t); }
        else o.write(c);
    }
    o.write('"');
}

static void head(WiFiClient& c, int code, const char* status, const char* ctype, const char* extra = "") {
    c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n", code, status, ctype, extra);
}
static void plain(WiFiClient& c, int code, const char* status, const char* extra = "") {
    head(c, code, status, "text/plain", extra);
    c.print(status);
}

// ---- tiny JSON field finders (flat, first match; the requests are small and the sender is authenticated) --------------
static const char* jfind(const char* s, const char* key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = s;
    while ((p = strstr(p, pat)) != nullptr) {
        const char* q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { q++; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++; return q; }
        p = q;
    }
    return nullptr;
}
static bool jstr(const char* s, const char* key, char* out, size_t cap) {
    const char* v = jfind(s, key);
    if (!v || *v != '"') return false;
    v++;
    size_t n = 0;
    while (*v && *v != '"' && n + 1 < cap) { if (*v == '\\' && v[1]) v++; out[n++] = *v++; }
    out[n] = 0;
    return *v == '"';
}
static bool jint(const char* s, const char* key, long* out) {
    const char* v = jfind(s, key);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return false;
    *out = atol(v);
    return true;
}
static bool jbool(const char* s, const char* key, bool* out) {
    const char* v = jfind(s, key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}
// The request id, copied as it was sent (a number or a quoted string) so the reply can echo it.
static bool jid(const char* s, char* out, size_t cap) {
    const char* v = jfind(s, "id");
    if (!v) return false;
    size_t n = 0;
    if (*v == '"') {
        out[n++] = *v++;
        while (*v && n + 2 < cap) { if (*v == '\\' && v[1]) { out[n++] = *v++; } out[n++] = *v++; if (out[n - 1] == '"' && n > 1) break; }
        out[n] = 0;
        return n >= 2 && out[n - 1] == '"';
    }
    while ((*v == '-' || (*v >= '0' && *v <= '9')) && n + 1 < cap) out[n++] = *v++;
    out[n] = 0;
    return n > 0;
}

// ---- tools -----------------------------------------------------------------------------------------------------------
// A tool writes its JSON result to o and returns true, or writes a one-line reason and returns false (sent as isError).
typedef bool (*ToolFn)(Print& o, const char* args);
struct Tool { const char* id; const char* schema; ToolFn fn; };

static bool tool_get_status(Print& o, const char*) {
    LinkState st = link_state();
    o.printf("{\"fw\":\"%s\",\"uptime_ms\":%lu,\"free_heap\":%u,\"mode\":", FW_VERSION, (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
    jprint(o, mode_name(mode_get()));
    o.printf(",\"link\":{\"state\":\"%s\"", link_state_name(st));
    if (st == LINK_JOIN && link_join_state() == LJ_CONNECTED) {
        o.print(",\"ssid\":"); jprint(o, link_join_ssid());
        o.printf(",\"ip\":\"%s\",\"rssi\":%d", link_join_ip(), link_join_rssi());
    } else if (st == LINK_AP) {
        o.printf(",\"ssid\":\"%s\",\"channel\":%u,\"clients\":%d", link_ap_ssid(), (unsigned)link_ap_channel(), link_ap_clients());
    }
    o.printf(",\"idle_off_s\":%lu},\"c5\":{\"up\":%s", (unsigned long)link_idle_left_s(), s_c5 && s_c5->link_up ? "true" : "false");
    if (s_c5 && s_c5->link_up) {
        o.print(",\"fw\":"); jprint(o, s_c5->fw);
        o.printf(",\"battery_mv\":%u,\"scan\":%u,\"lock\":%u,\"band\":%u,\"detections\":%u", (unsigned)s_c5->batt_mv, (unsigned)s_c5->scan,
                 (unsigned)s_c5->lock, (unsigned)s_c5->band, (unsigned)s_c5->dets);
    }
    o.print("}}");
    return true;
}

// ---- the read tools: answer from the CYD's cache of what the C5 reported -----------------------------------------------
// The server cannot wait on the C5, so a cold cache answers {"pending":true,"retry_ms":...} and the caller asks again.
static bool arg_mac_mode(const char* args, ListMacMode* m, Print& o) {
    char v[8];
    *m = LMAC_FULL;
    if (!jstr(args, "mac_mode", v, sizeof(v))) return true;
    if (strcmp(v, "full") == 0) *m = LMAC_FULL;
    else if (strcmp(v, "oui") == 0) *m = LMAC_OUI;
    else if (strcmp(v, "none") == 0) *m = LMAC_NONE;
    else { o.print("mac_mode must be full, oui or none"); return false; }
    return true;
}

static bool c5_ready(Print& o) {
    if (s_c5 && s_c5->link_up) return true;
    o.print("the C5 radio is not connected");
    return false;
}

static void pending(Print& o, const char* what) { o.printf("{\"pending\":true,\"retry_ms\":1500,\"waiting_for\":\"%s\"}", what); }

// "mac":"..." per the caller's mode (full, OUI with the device part masked, or left out)
static void mac_field(Print& o, const char* mac, ListMacMode mode) {
    if (mode == LMAC_NONE) return;
    if (mode == LMAC_FULL) { o.print("\"mac\":"); jprint(o, mac); return; }
    o.printf("\"mac\":\"%.8s:xx:xx:xx\"", mac);
}

static bool tool_list_detections(Print& o, const char* args) {
    ListMacMode mm;
    if (!arg_mac_mode(args, &mm, o)) return false;
    bool alerts = false;
    long lim = 20;
    jbool(args, "alerts_only", &alerts);
    if (jint(args, "limit", &lim)) lim = lim < 1 ? 1 : lim > MODEL_MAX_DETS ? MODEL_MAX_DETS : lim;
    if (!c5_ready(o)) return false;
    if (!g_model.list_ms) { pending(o, "detection list"); return true; }
    uint8_t idx[MODEL_MAX_DETS];
    int n = 0;
    for (int i = 0; i < g_model.ndets; i++) {                              // strongest first
        const Det& d = g_model.dets[i];
        if (!d.used || (alerts && d.best < CONF_ALERT_MIN)) continue;
        int j = n++;
        while (j > 0 && g_model.dets[idx[j - 1]].best < d.best) { idx[j] = idx[j - 1]; j--; }
        idx[j] = (uint8_t)i;
    }
    if (n > lim) n = (int)lim;
    listexport_json_head(o, mm, alerts ? "mcp alerts_only" : "mcp", n);
    for (int k = 0; k < n; k++) listexport_json_record(o, g_model.dets[idx[k]], mm, k == 0);
    o.print("]}");
    return true;
}

static const char* const SEC_NAMES[] = { "unknown", "open", "wep", "wpa", "wpa2", "wpa2_wpa3", "wpa3", "wpa2_enterprise", "wpa3_enterprise", "owe" };
static const char* const ATYPE_NAMES[] = { "unknown", "ble_public", "ble_random_static", "ble_rpa", "ble_nrpa", "wifi_universal", "wifi_local" };

static bool tool_scan_wifi(Print& o, const char* args) {
    ListMacMode mm;
    if (!arg_mac_mode(args, &mm, o)) return false;
    if (!c5_ready(o)) return false;
    s_aps_until = millis() + WANT_MS;                                      // the screens only poll the AP table while the scope shows it
    uint32_t now = millis();
    if (!g_model.aps_ms || now - g_model.aps_ms > 10000) { pending(o, "access point table"); return true; }
    uint8_t idx[MODEL_MAX_APS];
    int n = 0;
    for (int i = 0; i < MODEL_MAX_APS; i++) {                              // strongest first
        const ApRow& a = g_model.aps[i];
        if (!a.used) continue;
        int j = n++;
        while (j > 0 && g_model.aps[idx[j - 1]].rssi < a.rssi) { idx[j] = idx[j - 1]; j--; }
        idx[j] = (uint8_t)i;
    }
    o.printf("{\"age_ms\":%lu,\"count\":%d,\"access_points\":[", (unsigned long)(now - g_model.aps_ms), n);
    for (int k = 0; k < n; k++) {
        const ApRow& a = g_model.aps[idx[k]];
        o.print(k ? ",{" : "{");
        if (mm != LMAC_NONE) { mac_field(o, a.mac, mm); o.print(","); }
        o.print("\"ssid\":"); jprint(o, a.ssid);
        o.printf(",\"channel\":%u,\"band\":%u,\"security\":\"%s\",\"rssi\":%d,\"age_ms\":%lu}", a.ch, a.band, SEC_NAMES[a.sec <= SEC_OWE ? a.sec : 0],
                 (int)a.rssi, (unsigned long)(now - a.last_ms));
    }
    o.print("]}");
    return true;
}

static bool tool_scan_ble(Print& o, const char* args) {
    ListMacMode mm;
    if (!arg_mac_mode(args, &mm, o)) return false;
    if (!c5_ready(o)) return false;
    uint32_t now = millis();                                                // the BLE table is polled in the background all the time
    if (!g_model.ble_ms || now - g_model.ble_ms > 15000) { pending(o, "BLE table"); return true; }
    int n = 0;
    for (int i = 0; i < MODEL_MAX_BLE; i++) if (g_model.ble[i].used) n++;
    o.printf("{\"age_ms\":%lu,\"heard\":%u,\"count\":%d,\"devices\":[", (unsigned long)(now - g_model.ble_ms), (unsigned)g_model.ble_total, n);
    int k = 0;
    for (int i = 0; i < MODEL_MAX_BLE; i++) {
        const BleRow& b = g_model.ble[i];
        if (!b.used) continue;
        o.print(k++ ? ",{" : "{");
        if (mm != LMAC_NONE) { mac_field(o, b.mac, mm); o.print(","); }
        o.print("\"name\":"); jprint(o, b.name);
        if (b.has_company) o.printf(",\"company_id\":%u", (unsigned)b.company);
        o.printf(",\"rssi\":%d,\"age_ms\":%lu}", (int)b.rssi, (unsigned long)(now - b.last_ms));
    }
    o.print("]}");
    return true;
}

static void deg(Print& o, const char* key, int32_t e6) { o.printf(",\"%s\":%.6f", key, e6 / 1e6); }

static bool tool_get_device_profile(Print& o, const char* args) {
    char want[24];
    if (!jstr(args, "mac", want, sizeof(want))) { o.print("mac is required (take it from list_detections)"); return false; }
    if (!c5_ready(o)) return false;
    const Det* d = nullptr;                                                // only a MAC the table holds: the caller cannot make the C5 chase an arbitrary address
    for (int i = 0; i < g_model.ndets; i++) if (g_model.dets[i].used && strcasecmp(g_model.dets[i].mac, want) == 0) { d = &g_model.dets[i]; break; }
    if (!d) { o.print("no such detection (see list_detections)"); return false; }
    char shown[18];
    if (ui_wants_profile(shown) && strcmp(shown, d->mac) != 0) { o.print("the INFO screen is showing another detection; leave it and retry"); return false; }
    strncpy(s_prof_mac, d->mac, sizeof(s_prof_mac) - 1);
    s_prof_mac[sizeof(s_prof_mac) - 1] = 0;
    s_prof_until = millis() + WANT_MS;
    const Profile& p = g_profile;
    if (!p.have || strcmp(p.mac, d->mac) != 0) { pending(o, "device profile"); return true; }
    o.print("{\"mac\":"); jprint(o, p.mac);
    o.printf(",\"age_ms\":%lu,\"address_type\":\"%s\",\"rssi_min\":%d,\"rssi_max\":%d,\"seen_on_both_radios\":%s", (unsigned long)(millis() - p.got_ms),
             ATYPE_NAMES[p.atype <= AT_WIFI_LOCAL ? p.atype : 0], (int)p.rmin, (int)p.rmax, p.corr ? "true" : "false");
    if (p.dev_name[0]) { o.print(",\"name\":"); jprint(o, p.dev_name); }
    if (p.company >= 0) o.printf(",\"company_id\":%ld", (long)p.company);
    if (p.has_txp) o.printf(",\"tx_power_dbm\":%d", (int)p.txp);
    if (p.has_appear) o.printf(",\"appearance\":%u", (unsigned)p.appear);
    if (p.has_apple) o.printf(",\"apple\":{\"type\":%u,\"info\":%u}", (unsigned)p.apple, (unsigned)p.apple_i);
    if (p.nsvc) {
        o.print(",\"services\":[");
        for (int i = 0; i < p.nsvc && i < 4; i++) o.printf("%s\"0x%04x\"", i ? "," : "", (unsigned)p.svc[i]);
        o.print("]");
    }
    if (p.probe[0]) { o.print(",\"probe_ssid\":"); jprint(o, p.probe); }
    if (p.wifi) {
        o.printf(",\"wifi\":{\"security\":\"%s\",\"standards\":[", SEC_NAMES[p.wsec <= SEC_OWE ? p.wsec : 0]);
        const char* sep = "";
        if (p.wstd & WSTD_N)  { o.printf("%s\"n\"", sep);  sep = ","; }
        if (p.wstd & WSTD_AC) { o.printf("%s\"ac\"", sep); sep = ","; }
        if (p.wstd & WSTD_AX) { o.printf("%s\"ax\"", sep); sep = ","; }
        if (p.wstd & WSTD_BE) { o.printf("%s\"be\"", sep); sep = ","; }
        o.printf("],\"beacon_interval\":%u,\"hidden_ssid\":%s", (unsigned)p.bint, p.hidden ? "true" : "false");
        if (p.country[0]) { o.print(",\"country\":"); jprint(o, p.country); }
        o.print("}");
    }
    if (p.rid) {
        o.printf(",\"remote_id\":{\"ua_type\":%u", (unsigned)p.uat);
        if (p.uas[0]) { o.print(",\"uas_id\":"); jprint(o, p.uas); }
        if (p.op[0]) { o.print(",\"operator_id\":"); jprint(o, p.op); }
        if (p.loc) { deg(o, "lat", p.lat_e6); deg(o, "lon", p.lon_e6); }
        if (p.has_h)   o.printf(",\"height_m\":%.1f", p.h_dm / 10.0);
        if (p.has_alt) o.printf(",\"altitude_m\":%.1f", p.alt_dm / 10.0);
        if (p.has_spd) o.printf(",\"speed_ms\":%.2f", p.spd_cms / 100.0);
        if (p.has_vs)  o.printf(",\"vertical_speed_ms\":%.2f", p.vs_cms / 100.0);
        if (p.has_hdg) o.printf(",\"heading_deg\":%ld", (long)p.hdg);
        if (p.op_pos) { deg(o, "operator_lat", p.olat_e6); deg(o, "operator_lon", p.olon_e6); }
        o.print("}");
    }
    o.print("}");
    return true;
}

static const char* const SCHEMA_NONE = "{\"type\":\"object\",\"properties\":{}}";
#define MAC_MODE_PROP "\"mac_mode\":{\"type\":\"string\",\"enum\":[\"full\",\"oui\",\"none\"],\"description\":\"how much of each MAC to return; default full\"}"
static const char* const SCHEMA_LIST =
    "{\"type\":\"object\",\"properties\":{\"alerts_only\":{\"type\":\"boolean\",\"description\":\"only detections at alert strength\"},"
    MAC_MODE_PROP ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":40,\"description\":\"default 20, strongest first\"}}}";
static const char* const SCHEMA_SCAN = "{\"type\":\"object\",\"properties\":{" MAC_MODE_PROP "}}";
static const char* const SCHEMA_PROFILE =
    "{\"type\":\"object\",\"properties\":{\"mac\":{\"type\":\"string\",\"description\":\"a MAC from list_detections\"}},\"required\":[\"mac\"]}";

// Only registry rows that have a handler here are offered by tools/list.
static const Tool TOOLS[] = {
    { "get_status",         SCHEMA_NONE,    tool_get_status },
    { "list_detections",    SCHEMA_LIST,    tool_list_detections },
    { "scan_wifi",          SCHEMA_SCAN,    tool_scan_wifi },
    { "scan_ble",           SCHEMA_SCAN,    tool_scan_ble },
    { "get_device_profile", SCHEMA_PROFILE, tool_get_device_profile },
};
static const int NTOOLS = (int)(sizeof(TOOLS) / sizeof(TOOLS[0]));

static const Tool* find_tool(const char* id) {
    for (int i = 0; i < NTOOLS; i++) if (strcmp(TOOLS[i].id, id) == 0) return &TOOLS[i];
    return nullptr;
}
static const ss_cap_t* find_cap(const char* id) {
    for (size_t i = 0; i < SS_CAP_COUNT; i++) if (strcmp(SS_CAP_TABLE[i].id, id) == 0) return &SS_CAP_TABLE[i];
    return nullptr;
}

// ---- JSON-RPC --------------------------------------------------------------------------------------------------------
static void rpc_error(WiFiClient& c, const char* id, int code, const char* msg) {
    head(c, 200, "OK", "application/json");
    OutBuf o(c);
    o.printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id, code);
    jprint(o, msg);
    o.print("}}");
    o.flush();
}

static void rpc_dispatch(WiFiClient& c, const char* body) {
    char id[32];
    bool has_id = jid(body, id, sizeof(id));
    char method[32];
    if (!jstr(body, "method", method, sizeof(method))) {
        if (has_id) rpc_error(c, id, -32600, "invalid request");
        else plain(c, 400, "Bad Request");
        return;
    }
    if (!has_id) { head(c, 202, "Accepted", "text/plain", "Content-Length: 0\r\n"); return; }                // a notification (notifications/initialized, ...): no reply body

    if (strcmp(method, "initialize") == 0) {
        char ver[16] = "2025-03-26";
        char want[16];
        if (jstr(body, "protocolVersion", want, sizeof(want)) && strlen(want) == 10 && want[4] == '-' && want[7] == '-') strcpy(ver, want);   // echo the client's date-style version
        head(c, 200, "OK", "application/json");
        OutBuf o(c);
        o.printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},"
                 "\"serverInfo\":{\"name\":\"signalsweep-cyd\",\"version\":\"%s\"}}}", id, ver, FW_VERSION);
        o.flush();
        return;
    }
    if (strcmp(method, "ping") == 0) {
        head(c, 200, "OK", "application/json");
        OutBuf o(c);
        o.printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id);
        o.flush();
        return;
    }
    if (strcmp(method, "tools/list") == 0) {
        head(c, 200, "OK", "application/json");
        OutBuf o(c);
        o.printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":[", id);
        bool first = true;
        for (size_t i = 0; i < SS_CAP_COUNT; i++) {                      // the registry is the source; the handler table only filters
            const ss_cap_t& cap = SS_CAP_TABLE[i];
            const Tool* t = find_tool(cap.id);
            if (cap.cls != SS_CLS_PASSIVE || !t) continue;
            o.print(first ? "" : ",");
            first = false;
            o.print("{\"name\":"); jprint(o, cap.id);
            o.print(",\"description\":"); jprint(o, cap.mcp_desc);
            o.printf(",\"inputSchema\":%s}", t->schema);
        }
        o.print("]}}");
        o.flush();
        return;
    }
    if (strcmp(method, "tools/call") == 0) {
        const char* params = jfind(body, "params");
        const char* argp = params ? jfind(params, "arguments") : nullptr;
        char name[32] = {};
        // "name" belongs to params, not to anything inside arguments: look for it only before the arguments object
        char head_part[96] = {};
        if (params) {
            size_t lim = argp ? (size_t)(argp - params) : strlen(params);
            if (lim >= sizeof(head_part)) lim = sizeof(head_part) - 1;
            memcpy(head_part, params, lim);
        }
        if (!jstr(head_part, "name", name, sizeof(name))) { rpc_error(c, id, -32602, "tools/call needs a tool name"); return; }
        const Tool* t = find_tool(name);
        const ss_cap_t* cap = find_cap(name);
        if (!t || !cap) { rpc_error(c, id, -32602, "unknown tool"); return; }
        if (ss_cap_check(cap->opcode, mode_get(), false) != SS_OK) { rpc_error(c, id, -32001, "not allowed in the current mode"); return; }
        head(c, 200, "OK", "application/json");
        OutBuf o(c);
        o.printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"", id);
        EscOut e(o);
        bool ok = t->fn(e, argp ? argp : "");
        o.print(ok ? "\"}]}}" : "\"}],\"isError\":true}}");
        o.flush();
        return;
    }
    rpc_error(c, id, -32601, "method not found");
}

// ---- HTTP ------------------------------------------------------------------------------------------------------------
// One line (without the line ending), or -1 on timeout/disconnect. Over-long lines are cut, not rejected.
static int read_line(WiFiClient& c, char* buf, size_t cap, uint32_t deadline) {
    size_t n = 0;
    while ((int32_t)(deadline - millis()) > 0) {
        if (!c.available()) { if (!c.connected()) return -1; delay(1); continue; }
        int ch = c.read();
        if (ch == '\n') { buf[n] = 0; return (int)n; }
        if (ch == '\r') continue;
        if (n + 1 < cap) buf[n++] = (char)ch;
    }
    return -1;
}

static bool bearer_ok(const char* v) {
    if (strncmp(v, "Bearer ", 7) != 0) return false;
    v += 7;
    const char* tok = link_token();
    size_t a = strlen(v), b = strlen(tok);
    uint8_t diff = (uint8_t)(a != b);
    for (size_t i = 0; i < b; i++) diff |= (uint8_t)(v[i < a ? i : 0] ^ tok[i]);       // no early exit on the first wrong character
    return diff == 0;
}

static void serve(WiFiClient& c) {
    uint32_t deadline = millis() + MCP_IO_MS;
    char line[160];
    if (read_line(c, line, sizeof(line), deadline) <= 0) { Serial.println("[mcp] no request line within the time limit"); return; }
    char* sp = strchr(line, ' ');
    if (!sp) { plain(c, 400, "Bad Request"); return; }
    *sp = 0;
    char method[8], path[32];                                             // copies: the header loop below reuses `line`
    strlcpy(method, line, sizeof(method));
    char* sp2 = strchr(sp + 1, ' ');
    if (sp2) *sp2 = 0;
    strlcpy(path, sp + 1, sizeof(path));

    long clen = -1;
    bool auth = false;
    for (int h = 0;; h++) {
        int n = read_line(c, line, sizeof(line), deadline);
        if (n < 0) return;
        if (n == 0) break;                                                // the blank line ends the headers
        if (h >= 32) { plain(c, 431, "Request Header Fields Too Large"); return; }
        if (strncasecmp(line, "content-length:", 15) == 0) clen = atol(line + 15);
        else if (strncasecmp(line, "authorization:", 14) == 0) { const char* v = line + 14; while (*v == ' ') v++; auth = bearer_ok(v); }
    }

    if (strcmp(path, "/mcp") != 0) { plain(c, 404, "Not Found"); return; }
    if (strcmp(method, "POST") != 0) { plain(c, 405, "Method Not Allowed", "Allow: POST\r\n"); return; }
    uint32_t now = millis();
    if (s_locked && (int32_t)(s_lock_until - now) > 0) { plain(c, 429, "Too Many Requests", "Retry-After: 30\r\n"); return; }
    s_locked = false;
    if (!auth) {                                                          // checked before any body is read
        if (++s_bad >= LOCK_TRIES) { s_bad = 0; s_locked = true; s_lock_until = now + LOCK_MS; Serial.println("[mcp] too many bad tokens: locked out for 30 s"); }
        plain(c, 401, "Unauthorized", "WWW-Authenticate: Bearer\r\n");
        return;
    }
    s_bad = 0;
    link_activity();                                                      // a real request: the idle timer starts over
    if (clen < 1) { plain(c, 411, "Length Required"); return; }
    if (clen > MCP_MAX_BODY) { plain(c, 413, "Payload Too Large"); return; }
    long got = 0;
    while (got < clen && (int32_t)(deadline + 1200 - millis()) > 0) {
        if (c.available()) s_body[got++] = (char)c.read();
        else if (!c.connected()) break;
        else delay(1);
    }
    if (got < clen) { plain(c, 408, "Request Timeout"); return; }
    s_body[got] = 0;
    rpc_dispatch(c, s_body);
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------
static bool should_run() {
    LinkState st = link_state();
    return (st == LINK_AP && link_radio_active()) || (st == LINK_JOIN && link_join_state() == LJ_CONNECTED);
}

void mcp_tick(uint32_t) {
    bool want = should_run();
    if (want && !s_up) {
        s_srv.begin();
        s_up = true;
        if (link_state() == LINK_JOIN) MDNS.addService("http", "tcp", MCP_PORT);      // the name signalsweep.local was set when the join finished
        Serial.printf("[mcp] listening on port %d, free heap %u KB\n", MCP_PORT, (unsigned)(ESP.getFreeHeap() / 1024));
    } else if (!want && s_up) {
        s_srv.end();
        s_up = false;
        Serial.println("[mcp] stopped");
    }
    if (!s_up) return;
    WiFiClient c = s_srv.available();
    if (!c) return;
    c.setNoDelay(true);                                                   // send each write at once: the header and the body are separate writes, and Nagle holds the second behind the first's ACK
    serve(c);
    c.flush();
    c.stop();
}
