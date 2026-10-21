// SPDX-License-Identifier: GPL-3.0-or-later
// opennet.cpp — see opennet.h for exactly what is and is not read.
#include "opennet.h"
#include "focus.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <strings.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_en = false;
static OnPeer s_peers[ON_MAX_PEERS];
static OnPeer s_tmp;                                   // what one frame told us (Wi-Fi task only)

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void opennet_clear(void) {
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < ON_MAX_PEERS; i++) s_peers[i] = OnPeer();
    portEXIT_CRITICAL(&s_mux);
}

void opennet_set_enabled(bool on) {
    if (s_en && !on) opennet_clear();
    s_en = on;
}

bool opennet_enabled(void) { return s_en; }

// ---- small helpers -------------------------------------------------------------------------------
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Copy printable ASCII only, stop at the first byte that is not.
static void cpy(char* d, size_t cap, const uint8_t* s, size_t n) {
    size_t k = 0;
    while (k < n && k + 1 < cap && s[k] >= 0x20 && s[k] <= 0x7E) { d[k] = (char)s[k]; k++; }
    d[k] = 0;
}

static bool add_str(char (*arr)[ON_NAME], uint8_t* cnt, uint8_t maxn, const char* s) {
    if (!*s) return false;
    for (uint8_t i = 0; i < *cnt; i++) if (strcasecmp(arr[i], s) == 0) return true;
    if (*cnt >= maxn) return false;
    strncpy(arr[*cnt], s, ON_NAME - 1);
    arr[*cnt][ON_NAME - 1] = 0;
    (*cnt)++;
    return true;
}

// ---- DHCP (client messages only): hostname (12) and vendor class (60) ----------------------------
static void parse_dhcp(const uint8_t* p, int n) {
    if (n < 241 || p[0] != 1) return;                          // BOOTREQUEST only: a client speaking about itself
    if (p[236] != 0x63 || p[237] != 0x82 || p[238] != 0x53 || p[239] != 0x63) return;
    int i = 240;
    while (i < n) {
        uint8_t code = p[i++];
        if (code == 0) continue;
        if (code == 255 || i >= n) break;
        uint8_t l = p[i++];
        if (i + l > n) break;
        if (code == 12) cpy(s_tmp.host, sizeof(s_tmp.host), p + i, l);
        else if (code == 60) cpy(s_tmp.vclass, sizeof(s_tmp.vclass), p + i, l);
        i += l;
    }
}

// ---- mDNS responses: the names a device announces for itself -------------------------------------
// Reads a (possibly compressed) DNS name into out as dotted text. Returns the offset after the name
// at its original position, or -1 on anything malformed.
static int dns_name(const uint8_t* m, int ml, int o, char* out, int cap) {
    int ret = -1, hops = 0, w = 0;
    for (;;) {
        if (o < 0 || o >= ml) return -1;
        uint8_t c = m[o];
        if (c == 0) { if (ret < 0) ret = o + 1; break; }
        if ((c & 0xC0) == 0xC0) {
            if (o + 1 >= ml) return -1;
            if (ret < 0) ret = o + 2;
            if (++hops > 8) return -1;
            o = ((c & 0x3F) << 8) | m[o + 1];
            continue;
        }
        if (c & 0xC0) return -1;
        if (o + 1 + c > ml) return -1;
        if (w && w + 1 < cap) out[w++] = '.';
        for (int k = 0; k < c && w + 1 < cap; k++) {
            uint8_t b = m[o + 1 + k];
            out[w++] = (b >= 0x20 && b <= 0x7E) ? (char)b : '?';
        }
        o += 1 + c;
    }
    out[w] = 0;
    return ret;
}

static void first_label(const char* full, char* out, size_t cap) {
    size_t k = 0;
    while (full[k] && full[k] != '.' && k + 1 < cap) { out[k] = full[k]; k++; }
    out[k] = 0;
}

// "_airplay._tcp.local" -> "_airplay._tcp"
static void service_type(const char* full, char* out, size_t cap) {
    const char* d1 = strchr(full, '.');
    const char* d2 = d1 ? strchr(d1 + 1, '.') : nullptr;
    size_t n = d2 ? (size_t)(d2 - full) : strlen(full);
    if (n >= cap) n = cap - 1;
    memcpy(out, full, n);
    out[n] = 0;
}

static void parse_mdns(const uint8_t* m, int ml) {
    if (ml < 12) return;
    uint16_t flags = be16(m + 2);
    if (!(flags & 0x8000)) return;                             // a query: never read
    int qd = be16(m + 4), an = be16(m + 6), ns = be16(m + 8), ar = be16(m + 10);
    int o = 12;
    char nm[64];
    for (int i = 0; i < qd && i < 4; i++) {                    // skip the question section
        o = dns_name(m, ml, o, nm, sizeof(nm));
        if (o < 0 || o + 4 > ml) return;
        o += 4;
    }
    int total = an + ns + ar;
    if (total > 12) total = 12;
    for (int i = 0; i < total; i++) {
        char own[64];
        o = dns_name(m, ml, o, own, sizeof(own));
        if (o < 0 || o + 10 > ml) return;
        uint16_t type = be16(m + o);
        int rdl = be16(m + o + 8);
        int rd = o + 10;
        if (rd + rdl > ml) return;
        char lab[ON_NAME];
        if (type == 12) {                                      // PTR: service type -> instance name
            if (strncmp(own, "_services", 9) != 0 && own[0] == '_' && s_tmp.nsvc < ON_NSVC) {
                char st[ON_SVC];
                service_type(own, st, sizeof(st));
                bool dup = false;
                for (uint8_t k = 0; k < s_tmp.nsvc; k++) if (strcmp(s_tmp.svc[k], st) == 0) dup = true;
                if (!dup) { strncpy(s_tmp.svc[s_tmp.nsvc], st, ON_SVC - 1); s_tmp.nsvc++; }
            }
            char inst[64];
            if (dns_name(m, ml, rd, inst, sizeof(inst)) >= 0) {
                first_label(inst, lab, sizeof(lab));
                if (lab[0] != '_') add_str(s_tmp.name, &s_tmp.nnames, ON_NNAME, lab);
            }
        } else if (type == 33 || type == 1 || type == 28) {    // SRV / A / AAAA: the owner is the device's own name
            first_label(own, lab, sizeof(lab));
            if (lab[0] != '_') add_str(s_tmp.name, &s_tmp.nnames, ON_NNAME, lab);
        }                                                      // TXT and everything else: skipped
        o = rd + rdl;
    }
}

// ---- SSDP NOTIFY: the SERVER header only ----------------------------------------------------------
static void parse_ssdp(const uint8_t* p, int n) {
    if (n < 8 || memcmp(p, "NOTIFY ", 7) != 0) return;         // M-SEARCH is a query: never read
    int i = 0;
    while (i < n) {
        int e = i;
        while (e < n && p[e] != '\n') e++;
        int len = e - i;
        if (len > 0 && p[i + len - 1] == '\r') len--;
        if (len == 0) break;                                   // blank line: end of the headers
        if (len > 7 && strncasecmp((const char*)p + i, "SERVER:", 7) == 0) {
            int v = i + 7;
            while (v < i + len && p[v] == ' ') v++;
            cpy(s_tmp.ssdp, sizeof(s_tmp.ssdp), p + v, (size_t)(i + len - v));
            break;
        }
        i = e + 1;
    }
}

// ---- merge what one frame told us into the table ---------------------------------------------------
static void bump_port(OnPeer& d, uint16_t port, uint8_t proto) {
    int free_i = -1, low = 0;
    for (int i = 0; i < ON_NPORT; i++) {
        if (d.ports[i].n && d.ports[i].port == port && d.ports[i].proto == proto) { d.ports[i].n++; return; }
        if (!d.ports[i].n) { if (free_i < 0) free_i = i; }
        else if (d.ports[i].n < d.ports[low].n) low = i;
    }
    int slot = free_i >= 0 ? free_i : low;
    d.ports[slot].port = port; d.ports[slot].proto = proto; d.ports[slot].n = 1;
}

static void merge(const uint8_t sa[6], uint8_t proto, uint16_t dport) {
    uint32_t now = ms_now();
    portENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < ON_MAX_PEERS; i++) if (s_peers[i].used && memcmp(s_peers[i].mac, sa, 6) == 0) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < ON_MAX_PEERS; i++) if (!s_peers[i].used) { slot = i; break; }
        if (slot < 0) {                                        // full: replace the stalest
            slot = 0;
            for (int i = 1; i < ON_MAX_PEERS; i++) if (s_peers[i].last_ms < s_peers[slot].last_ms) slot = i;
        }
        s_peers[slot] = OnPeer();
        s_peers[slot].used = true;
        memcpy(s_peers[slot].mac, sa, 6);
    }
    OnPeer& d = s_peers[slot];
    d.last_ms = now;
    if (s_tmp.host[0]) memcpy(d.host, s_tmp.host, sizeof(d.host));
    if (s_tmp.vclass[0]) memcpy(d.vclass, s_tmp.vclass, sizeof(d.vclass));
    if (s_tmp.ssdp[0]) memcpy(d.ssdp, s_tmp.ssdp, sizeof(d.ssdp));
    for (uint8_t i = 0; i < s_tmp.nnames; i++) add_str(d.name, &d.nnames, ON_NNAME, s_tmp.name[i]);
    for (uint8_t i = 0; i < s_tmp.nsvc; i++) {
        bool dup = false;
        for (uint8_t k = 0; k < d.nsvc; k++) if (strcmp(d.svc[k], s_tmp.svc[i]) == 0) dup = true;
        if (!dup && d.nsvc < ON_NSVC) { memcpy(d.svc[d.nsvc], s_tmp.svc[i], ON_SVC); d.nsvc++; }
    }
    if (proto == 6) { d.tcp++; bump_port(d, dport, 6); }
    else if (proto == 17) { d.udp++; bump_port(d, dport, 17); }
    else if (proto == 1) d.icmp++;
    else d.other++;
    portEXIT_CRITICAL(&s_mux);
}

// ---- the entry point -------------------------------------------------------------------------------
void opennet_note(const uint8_t* f, int length) {
    if (!s_en || length < 48) return;
    if (((f[0] >> 2) & 3) != 2) return;                        // data frames only
    uint8_t sub = f[0] >> 4;
    if (sub & 0x04) return;                                    // null-function frames carry no data
    if (f[1] & 0x40) return;                                   // Protected: encrypted, never read
    bool to_ds = f[1] & 0x01, from_ds = f[1] & 0x02;
    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
    if (!focus_matches(a1) && !focus_matches(a2) && !focus_matches(a3)) return;   // the focused target's traffic only
    const uint8_t* sa = (to_ds && from_ds) ? f + 24 : from_ds ? a3 : a2;
    int hdr = 24 + ((to_ds && from_ds) ? 6 : 0) + ((sub & 0x08) ? 2 : 0) + (((sub & 0x08) && (f[1] & 0x80)) ? 4 : 0);
    int avail = length - 4;                                    // sig_len includes the FCS
    if (avail < hdr + 8 + 20) return;
    const uint8_t* l = f + hdr;
    if (l[0] != 0xAA || l[1] != 0xAA || l[2] != 0x03 || l[3] || l[4] || l[5]) return;   // LLC/SNAP
    if (be16(l + 6) != 0x0800) return;                         // IPv4 only
    const uint8_t* ip = l + 8;
    int iplen = avail - hdr - 8;
    if ((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > iplen) return;
    if (be16(ip + 6) & 0x1FFF) return;                         // not the first fragment
    uint8_t proto = ip[9];
    uint16_t dport = 0;
    s_tmp = OnPeer();
    if ((proto == 6 || proto == 17) && iplen >= ihl + 4) {
        uint16_t sp = be16(ip + ihl), dp = be16(ip + ihl + 2);
        dport = dp;
        if (proto == 17 && iplen >= ihl + 8 && dp != 53 && sp != 53) {     // port 53 (DNS) is never parsed
            const uint8_t* pl = ip + ihl + 8;
            int pn = iplen - ihl - 8;
            if ((sp == 68 && dp == 67) || (sp == 67 && dp == 68)) parse_dhcp(pl, pn);
            else if (dp == 5353) parse_mdns(pl, pn);
            else if (dp == 1900) parse_ssdp(pl, pn);
        }
    }
    merge(sa, proto, dport);
}

int opennet_snapshot(OnPeer* out, int max) {
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < ON_MAX_PEERS && n < max; i++) if (s_peers[i].used) out[n++] = s_peers[i];
    portEXIT_CRITICAL(&s_mux);
    for (int i = 1; i < n; i++) {                              // busiest first
        OnPeer k = out[i];
        uint32_t kt = k.tcp + k.udp + k.icmp + k.other;
        int j = i - 1;
        while (j >= 0 && out[j].tcp + out[j].udp + out[j].icmp + out[j].other < kt) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return n;
}
