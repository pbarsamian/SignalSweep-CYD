// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.h"
#include "ss_cbor.h"
#include "theme.h"

Model g_model;
Profile g_profile;

static void copy_text(char* dst, size_t cap, const ss_cbor_val_t& v) {
    size_t n = v.n < cap - 1 ? v.n : cap - 1;
    if (v.ptr && n) memcpy(dst, v.ptr, n);
    dst[n] = 0;
}

Det* model_find(const char* mac) {
    for (int i = 0; i < g_model.ndets; i++)
        if (g_model.dets[i].used && strcmp(g_model.dets[i].mac, mac) == 0) return &g_model.dets[i];
    return nullptr;
}

int model_ingest_det(const uint8_t* cb, size_t n, bool* is_new, bool* alert) {
    ss_cbor_val_t mac, name, type, conf, best, rssi, ch, band, radio, age, seen, ssid;
    if (!ss_cbor_map_get(cb, n, "mac", &mac) || mac.type != SS_CBOR_TEXT) return -1;
    if (!ss_cbor_map_get(cb, n, "name", &name) || !ss_cbor_map_get(cb, n, "type", &type)) return -1;
    char m[18];
    copy_text(m, sizeof(m), mac);

    bool created = false;
    Det* d = model_find(m);
    if (!d) {
        if (g_model.ndets >= MODEL_MAX_DETS) return -1;
        d = &g_model.dets[g_model.ndets++];
        *d = Det();
        d->used = true;
        strncpy(d->mac, m, sizeof(d->mac) - 1);
        d->first_ms = millis();
        created = true;
    }
    uint32_t now = millis();
    uint32_t age_ms = ss_cbor_map_get(cb, n, "age_ms", &age) && age.type == SS_CBOR_UINT ? (uint32_t)age.u : 0;
    copy_text(d->name, sizeof(d->name), name);
    d->type = type.u > 5 ? 5 : (uint8_t)type.u;
    if (ss_cbor_map_get(cb, n, "conf", &conf)) d->conf = (uint8_t)conf.u;
    if (ss_cbor_map_get(cb, n, "best", &best)) d->best = (uint8_t)best.u;
    if (ss_cbor_map_get(cb, n, "rssi", &rssi)) d->rssi = (int8_t)rssi.i;
    if (ss_cbor_map_get(cb, n, "ch", &ch)) d->ch = (uint8_t)ch.u;
    if (ss_cbor_map_get(cb, n, "band", &band)) d->band = (uint8_t)band.u;
    if (ss_cbor_map_get(cb, n, "radio", &radio) && radio.type == SS_CBOR_TEXT)
        d->radio = (radio.n == 4) ? 2 : (radio.n == 3 && radio.ptr[0] == 'b') ? 1 : 0;   // "both" / "ble" / "wifi"
    if (ss_cbor_map_get(cb, n, "seen", &seen)) d->seen = (uint16_t)seen.u;
    if (ss_cbor_map_get(cb, n, "ssid", &ssid) && ssid.type == SS_CBOR_TEXT) copy_text(d->ssid, sizeof(d->ssid), ssid);
    d->last_ms = now - age_ms;
    d->sweep = g_model.sweep;

    bool ev_new = false, ev_alert = false;
    ss_cbor_get_bool(cb, n, "new", &ev_new);
    ss_cbor_get_bool(cb, n, "alert", &ev_alert);
    if (is_new) *is_new = ev_new || created;
    if (alert) *alert = ev_alert;
    return (int)(d - g_model.dets);
}

uint16_t model_ingest_list(const uint8_t* cb, size_t n) {
    uint32_t total = 0, offset = 0;
    ss_cbor_get_uint(cb, n, "total", &total);
    ss_cbor_get_uint(cb, n, "offset", &offset);
    if (offset == 0) g_model.sweep++;                    // a new sweep begins with the first page
    g_model.list_total = (uint16_t)total;

    ss_cbor_val_t arr;
    uint32_t got = 0;
    if (ss_cbor_map_get(cb, n, "items", &arr) && arr.type == SS_CBOR_ARRAY) {
        const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
        for (size_t i = 0; i < arr.n; i++) {
            const uint8_t* item = p;
            bool nw, al;
            model_ingest_det(item, end - item, &nw, &al);   // events carry the alert; a list page does not
            got++;
            if (!ss_cbor_skip(&p, end)) break;
        }
    }
    uint32_t next = offset + got;
    if (got > 0 && next < total) return (uint16_t)next;  // more pages to fetch

    // Sweep complete: drop entries the C5 no longer lists
    for (int i = 0; i < g_model.ndets; ) {
        if (g_model.dets[i].sweep != g_model.sweep && strcmp(g_model.dets[i].mac, g_focus_keep) != 0) {   // a focused peer may not be in the C5's list
            for (int j = i; j < g_model.ndets - 1; j++) g_model.dets[j] = g_model.dets[j + 1];
            g_model.ndets--;
            g_model.dets[g_model.ndets] = Det();
        } else i++;
    }
    g_model.list_ms = millis();
    return 0;
}

void model_ingest_aps(const uint8_t* cb, size_t n) {
    ss_cbor_val_t arr;
    if (!ss_cbor_map_get(cb, n, "aps", &arr) || arr.type != SS_CBOR_ARRAY) return;
    const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
    int cnt = 0;
    uint32_t now = millis();
    for (size_t i = 0; i < arr.n && cnt < MODEL_MAX_APS; i++) {
        const uint8_t* item = p;
        ss_cbor_val_t mac, ssid, ch, band, rssi, sec, age;
        if (ss_cbor_map_get(item, end - item, "ch", &ch) && ss_cbor_map_get(item, end - item, "rssi", &rssi) &&
            ss_cbor_map_get(item, end - item, "band", &band)) {
            ApRow& a = g_model.aps[cnt++];
            a = ApRow();
            a.used = true;
            a.ch = (uint8_t)ch.u; a.band = (uint8_t)band.u; a.rssi = (int8_t)rssi.i;
            if (ss_cbor_map_get(item, end - item, "mac", &mac)) copy_text(a.mac, sizeof(a.mac), mac);
            if (ss_cbor_map_get(item, end - item, "ssid", &ssid)) copy_text(a.ssid, sizeof(a.ssid), ssid);
            if (ss_cbor_map_get(item, end - item, "sec", &sec)) a.sec = (uint8_t)sec.u;
            uint32_t age_ms = ss_cbor_map_get(item, end - item, "age_ms", &age) ? (uint32_t)age.u : 0;
            a.last_ms = now - age_ms;
        }
        if (!ss_cbor_skip(&p, end)) break;
    }
    for (int i = cnt; i < MODEL_MAX_APS; i++) g_model.aps[i] = ApRow();
    g_model.naps = cnt;
    g_model.aps_ms = now;
}

void model_ingest_ble(const uint8_t* cb, size_t n) {
    ss_cbor_val_t arr;
    if (!ss_cbor_map_get(cb, n, "devs", &arr) || arr.type != SS_CBOR_ARRAY) return;
    uint32_t total = 0;
    ss_cbor_get_uint(cb, n, "total", &total);
    const uint8_t* p = arr.ptr; const uint8_t* end = cb + n;
    uint32_t now = millis();
    BleRow tmp[MODEL_MAX_BLE];
    int cnt = 0;
    for (size_t i = 0; i < arr.n && cnt < MODEL_MAX_BLE; i++) {
        const uint8_t* item = p;
        ss_cbor_val_t mac, name, rssi, company, age;
        if (ss_cbor_map_get(item, end - item, "mac", &mac) && ss_cbor_map_get(item, end - item, "rssi", &rssi)) {
            BleRow& r = tmp[cnt++];
            r = BleRow();
            r.used = true;
            copy_text(r.mac, sizeof(r.mac), mac);
            if (ss_cbor_map_get(item, end - item, "name", &name)) copy_text(r.name, sizeof(r.name), name);
            r.rssi = (int8_t)rssi.i;
            if (ss_cbor_map_get(item, end - item, "company", &company)) { r.has_company = true; r.company = (uint16_t)company.u; }
            uint32_t age_ms = ss_cbor_map_get(item, end - item, "age_ms", &age) ? (uint32_t)age.u : 0;
            r.last_ms = now - age_ms;
        }
        if (!ss_cbor_skip(&p, end)) break;
    }
    for (int i = 1; i < cnt; i++) {                      // strongest first
        BleRow k = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j].rssi < k.rssi) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    for (int i = 0; i < MODEL_MAX_BLE; i++) g_model.ble[i] = (i < cnt) ? tmp[i] : BleRow();
    g_model.nble = cnt;
    g_model.ble_total = (uint16_t)total;
    g_model.ble_ms = now;
}

FocusView g_fv;
HuntTick g_hunt;
char g_focus_keep[18] = {};

void model_focus_clear() {
    g_fv = FocusView();
    g_hunt = HuntTick();
}

static uint32_t get_u32(const uint8_t* cb, size_t n, const char* key) {
    ss_cbor_val_t v;
    return (ss_cbor_map_get(cb, n, key, &v) && v.type == SS_CBOR_UINT) ? (uint32_t)v.u : 0;
}

bool model_ingest_focus(const uint8_t* cb, size_t n) {
    ss_cbor_val_t v;
    if (!ss_cbor_map_get(cb, n, "mac", &v) || v.type != SS_CBOR_TEXT) return false;
    FocusView& f = g_fv;
    f = FocusView();
    f.have = true;
    copy_text(f.mac, sizeof(f.mac), v);
    f.wifi = ss_cbor_map_get(cb, n, "radio", &v) && v.type == SS_CBOR_TEXT && v.n == 4;
    f.ch = (uint8_t)get_u32(cb, n, "ch");
    f.got_ms = millis();
    f.up_ms = get_u32(cb, n, "up_ms");
    f.age_ms = get_u32(cb, n, "age_ms");
    if (ss_cbor_map_get(cb, n, "rssi", &v)) f.rssi = (int8_t)v.i;
    f.n = get_u32(cb, n, "n");
    f.beacons = get_u32(cb, n, "beacons");
    f.presp = get_u32(cb, n, "presp");
    f.preq = get_u32(cb, n, "preq");
    f.omgmt = get_u32(cb, n, "omgmt");
    f.data = get_u32(cb, n, "data");
    f.advs = get_u32(cb, n, "advs");
    f.adv_int = get_u32(cb, n, "adv_int");
    f.ht = (uint8_t)get_u32(cb, n, "ht");
    if (ss_cbor_map_get(cb, n, "iemap", &v) && v.type == SS_CBOR_BYTES && v.n == sizeof(f.iemap)) {
        memcpy(f.iemap, v.ptr, v.n);
        f.has_iemap = true;
    }
    if (ss_cbor_map_get(cb, n, "vie", &v) && v.type == SS_CBOR_BYTES) {
        f.nvie = (uint8_t)(v.n / 4 > 8 ? 8 : v.n / 4);
        memcpy(f.vie, v.ptr, (size_t)f.nvie * 4);
    }
    if (ss_cbor_map_get(cb, n, "raw", &v) && v.type == SS_CBOR_BYTES) {
        f.raw_len = (uint8_t)(v.n > sizeof(f.raw) ? sizeof(f.raw) : v.n);
        memcpy(f.raw, v.ptr, f.raw_len);
    }
    ss_cbor_val_t arr;
    if (ss_cbor_map_get(cb, n, "peers", &arr) && arr.type == SS_CBOR_ARRAY) {
        const uint8_t* p = arr.ptr;
        const uint8_t* end = cb + n;
        for (size_t i = 0; i < arr.n && f.npeers < FV_MAX_PEERS; i++) {
            const uint8_t* item = p;
            size_t il = end - item;
            ss_cbor_val_t pm;
            if (ss_cbor_map_get(item, il, "mac", &pm) && pm.type == SS_CBOR_TEXT) {
                FvPeer& q = f.peers[f.npeers++];
                copy_text(q.mac, sizeof(q.mac), pm);
                q.role = (uint8_t)get_u32(item, il, "role");
                ss_cbor_val_t r;
                if (ss_cbor_map_get(item, il, "rssi", &r)) q.rssi = (int8_t)r.i;
                q.tof = get_u32(item, il, "tof");
                q.frf = get_u32(item, il, "frf");
                q.tob = get_u32(item, il, "tob");
                q.frb = get_u32(item, il, "frb");
                q.qos = get_u32(item, il, "qos");
                q.age_ms = get_u32(item, il, "age_ms");
            }
            if (!ss_cbor_skip(&p, end)) break;
        }
    }
    if (ss_cbor_map_get(cb, n, "onet", &arr) && arr.type == SS_CBOR_ARRAY) {
        const uint8_t* p = arr.ptr;
        const uint8_t* end = cb + n;
        for (size_t i = 0; i < arr.n && f.nonet < 6; i++) {
            const uint8_t* item = p;
            size_t il = end - item;
            ss_cbor_val_t pm, v;
            if (ss_cbor_map_get(item, il, "mac", &pm) && pm.type == SS_CBOR_TEXT) {
                FvOnet& q = f.onet[f.nonet++];
                copy_text(q.mac, sizeof(q.mac), pm);
                q.tcp = get_u32(item, il, "tcp");
                q.udp = get_u32(item, il, "udp");
                q.icmp = get_u32(item, il, "icmp");
                if (ss_cbor_map_get(item, il, "h", &v) && v.type == SS_CBOR_TEXT) copy_text(q.host, sizeof(q.host), v);
                if (ss_cbor_map_get(item, il, "vc", &v) && v.type == SS_CBOR_TEXT) copy_text(q.vc, sizeof(q.vc), v);
                if (ss_cbor_map_get(item, il, "ss", &v) && v.type == SS_CBOR_TEXT) copy_text(q.ss, sizeof(q.ss), v);
                if (ss_cbor_map_get(item, il, "nm", &v) && v.type == SS_CBOR_ARRAY) {
                    const uint8_t* t = v.ptr;
                    for (size_t k = 0; k < v.n && q.nnm < 3; k++) {
                        if ((*t >> 5) == 3 && (*t & 0x1F) < 24) {                 // text, length in the head byte
                            size_t L = *t & 0x1F;
                            size_t c = L < sizeof(q.nm[0]) - 1 ? L : sizeof(q.nm[0]) - 1;
                            memcpy(q.nm[q.nnm], t + 1, c); q.nm[q.nnm][c] = 0; q.nnm++;
                        }
                        if (!ss_cbor_skip(&t, end)) break;
                    }
                }
                if (ss_cbor_map_get(item, il, "sv", &v) && v.type == SS_CBOR_ARRAY) {
                    const uint8_t* t = v.ptr;
                    for (size_t k = 0; k < v.n && q.nsv < 2; k++) {
                        if ((*t >> 5) == 3 && (*t & 0x1F) < 24) {
                            size_t L = *t & 0x1F;
                            size_t c = L < sizeof(q.sv[0]) - 1 ? L : sizeof(q.sv[0]) - 1;
                            memcpy(q.sv[q.nsv], t + 1, c); q.sv[q.nsv][c] = 0; q.nsv++;
                        }
                        if (!ss_cbor_skip(&t, end)) break;
                    }
                }
                if (ss_cbor_map_get(item, il, "pt", &v) && v.type == SS_CBOR_ARRAY) {
                    const uint8_t* t = v.ptr;
                    auto rd = [&](const uint8_t*& c) -> uint32_t {                // unsigned int, any width the C5 sends
                        uint8_t h = *c++;
                        if ((h >> 5) != 0) return 0;
                        uint8_t a = h & 0x1F;
                        if (a < 24) return a;
                        uint32_t r = 0;
                        int nb = a == 0x18 ? 1 : a == 0x19 ? 2 : 4;
                        for (int b = 0; b < nb; b++) r = (r << 8) | *c++;
                        return r;
                    };
                    for (size_t k = 0; k < v.n && q.npt < 4; k++) {
                        const uint8_t* c = t;
                        if (*c == 0x83) {                                          // [port, proto, count]
                            c++;
                            q.pt[q.npt][0] = (uint16_t)rd(c);
                            q.pt[q.npt][1] = (uint16_t)rd(c);
                            q.pt[q.npt][2] = (uint16_t)rd(c);
                            q.npt++;
                        }
                        if (!ss_cbor_skip(&t, end)) break;
                    }
                }
            }
            if (!ss_cbor_skip(&p, end)) break;
        }
    }
    return true;
}

const FvOnet* model_onet_for(const char* mac) {
    for (int i = 0; i < g_fv.nonet; i++) if (strcasecmp(g_fv.onet[i].mac, mac) == 0) return &g_fv.onet[i];
    return nullptr;
}

void model_ingest_tick(const uint8_t* cb, size_t n) {
    ss_cbor_val_t m, r;
    if (!ss_cbor_map_get(cb, n, "mac", &m) || m.type != SS_CBOR_TEXT) return;
    copy_text(g_hunt.mac, sizeof(g_hunt.mac), m);
    if (ss_cbor_map_get(cb, n, "rssi", &r)) g_hunt.rssi = (int8_t)r.i;
    uint32_t age = get_u32(cb, n, "age_ms");
    g_hunt.ever = age < 60000;
    g_hunt.heard_ms = millis() - age;
    Det* d = model_find(g_hunt.mac);                      // keep the focused entry's RSSI live (a pivoted peer is never re-listed)
    if (d && g_hunt.ever) { d->rssi = g_hunt.rssi; d->last_ms = g_hunt.heard_ms; }
}

Det* model_add_peer(const char* mac, uint8_t radio, uint8_t ch, int rssi) {
    Det* d = model_find(mac);
    if (d) return d;
    if (g_model.ndets >= MODEL_MAX_DETS) {                // full: drop an older placeholder (never the focused one)
        for (int i = 0; i < g_model.ndets; i++) {
            if (g_model.dets[i].type != 5 || strcmp(g_model.dets[i].mac, g_focus_keep) == 0) continue;
            for (int j = i; j < g_model.ndets - 1; j++) g_model.dets[j] = g_model.dets[j + 1];
            g_model.ndets--;
            g_model.dets[g_model.ndets] = Det();
            break;
        }
        if (g_model.ndets >= MODEL_MAX_DETS) return nullptr;
    }
    d = &g_model.dets[g_model.ndets++];
    *d = Det();
    d->used = true;
    strncpy(d->mac, mac, sizeof(d->mac) - 1);
    strncpy(d->name, "Peer (pivot)", sizeof(d->name) - 1);
    d->type = 5;
    d->radio = radio;
    d->ch = ch;
    d->band = radio == 1 ? 0 : (ch >= 36 ? 5 : 2);
    d->rssi = (int8_t)rssi;
    d->first_ms = d->last_ms = millis();
    d->sweep = g_model.sweep;
    return d;
}

bool model_ingest_profile(const uint8_t* cb, size_t n) {
    ss_cbor_val_t v, rid;
    if (!ss_cbor_map_get(cb, n, "mac", &v) || v.type != SS_CBOR_TEXT) return false;
    Profile& p = g_profile;
    p = Profile();
    copy_text(p.mac, sizeof(p.mac), v);
    p.have = true;
    p.got_ms = millis();
    uint32_t u = 0; bool b = false;
    if (ss_cbor_get_uint(cb, n, "atype", &u)) p.atype = (uint8_t)u;
    if (ss_cbor_map_get(cb, n, "rmin", &v)) p.rmin = (int8_t)v.i;
    if (ss_cbor_map_get(cb, n, "rmax", &v)) p.rmax = (int8_t)v.i;
    if (ss_cbor_get_bool(cb, n, "corr", &b)) p.corr = b;
    if (ss_cbor_map_get(cb, n, "dev_name", &v) && v.type == SS_CBOR_TEXT) copy_text(p.dev_name, sizeof(p.dev_name), v);
    if (ss_cbor_get_uint(cb, n, "company", &u)) p.company = (int32_t)u;
    if (ss_cbor_map_get(cb, n, "txp", &v)) { p.has_txp = true; p.txp = (int8_t)v.i; }
    if (ss_cbor_get_uint(cb, n, "appear", &u)) { p.has_appear = true; p.appear = (uint16_t)u; }
    if (ss_cbor_get_uint(cb, n, "apple", &u)) { p.has_apple = true; p.apple = (uint8_t)u; if (ss_cbor_get_uint(cb, n, "apple_i", &u)) p.apple_i = (uint8_t)u; }
    if (ss_cbor_map_get(cb, n, "svc", &v) && v.type == SS_CBOR_ARRAY) {
        const uint8_t* q = v.ptr; const uint8_t* end = cb + n;
        for (size_t i = 0; i < v.n && p.nsvc < 4; i++) {
            const uint8_t* it = q;                                    // items are bare unsigned integers
            uint8_t head = *it;
            if ((head >> 5) == 0) {                                   // major type 0
                uint32_t val = head & 0x1F;
                if (head == 0x18) val = it[1]; else if (head == 0x19) val = (it[1] << 8) | it[2];
                p.svc[p.nsvc++] = (uint16_t)val;
            }
            if (!ss_cbor_skip(&q, end)) break;
        }
    }
    if (ss_cbor_map_get(cb, n, "probe", &v) && v.type == SS_CBOR_TEXT) copy_text(p.probe, sizeof(p.probe), v);
    if (ss_cbor_get_uint(cb, n, "wsec", &u)) {
        p.wifi = true; p.wsec = (uint8_t)u;
        if (ss_cbor_get_uint(cb, n, "wstd", &u)) p.wstd = (uint8_t)u;
        if (ss_cbor_get_uint(cb, n, "bint", &u)) p.bint = (uint16_t)u;
        if (ss_cbor_get_bool(cb, n, "hidden", &b)) p.hidden = b;
    }
    if (ss_cbor_map_get(cb, n, "country", &v) && v.type == SS_CBOR_TEXT) copy_text(p.country, sizeof(p.country), v);
    if (ss_cbor_map_get(cb, n, "rid", &rid) && rid.type == SS_CBOR_MAP) {
        const uint8_t* m = rid.head; size_t ml = (cb + n) - m;      // the nested map, header included
        p.rid = true;
        if (ss_cbor_get_uint(m, ml, "uat", &u)) p.uat = (uint8_t)u;
        if (ss_cbor_map_get(m, ml, "uas", &v) && v.type == SS_CBOR_TEXT) copy_text(p.uas, sizeof(p.uas), v);
        if (ss_cbor_map_get(m, ml, "op", &v) && v.type == SS_CBOR_TEXT) copy_text(p.op, sizeof(p.op), v);
        ss_cbor_val_t a, o;
        if (ss_cbor_map_get(m, ml, "lat", &a) && ss_cbor_map_get(m, ml, "lon", &o)) { p.loc = true; p.lat_e6 = (int32_t)a.i; p.lon_e6 = (int32_t)o.i; }
        if (ss_cbor_map_get(m, ml, "h_dm", &a))    { p.has_h = true;   p.h_dm = (int32_t)a.i; }
        if (ss_cbor_map_get(m, ml, "alt_dm", &a))  { p.has_alt = true; p.alt_dm = (int32_t)a.i; }
        if (ss_cbor_map_get(m, ml, "spd_cms", &a)) { p.has_spd = true; p.spd_cms = (int32_t)a.i; }
        if (ss_cbor_map_get(m, ml, "vs_cms", &a))  { p.has_vs = true;  p.vs_cms = (int32_t)a.i; }
        if (ss_cbor_map_get(m, ml, "hdg", &a))     { p.has_hdg = true; p.hdg = (int32_t)a.i; }
        if (ss_cbor_map_get(m, ml, "olat", &a) && ss_cbor_map_get(m, ml, "olon", &o)) { p.op_pos = true; p.olat_e6 = (int32_t)a.i; p.olon_e6 = (int32_t)o.i; }
    }
    return true;
}

int model_visible_count() {
    int c = 0;
    for (int i = 0; i < g_model.ndets; i++) if (g_model.dets[i].used && g_model.dets[i].type != T_UNKNOWN) c++;
    return c;
}

int model_alert_count() {
    int c = 0;
    for (int i = 0; i < g_model.ndets; i++) {
        const Det& d = g_model.dets[i];
        if (d.used && d.type != T_UNKNOWN && d.best >= CONF_ALERT_MIN) c++;
    }
    return c;
}

void model_prune(uint32_t now_ms) {
    // Events can add entries between list sweeps; anything silent for 3 minutes is stale
    // regardless (the C5 prunes at 2 minutes and the next sweep would drop it anyway).
    for (int i = 0; i < g_model.ndets; ) {
        if (now_ms - g_model.dets[i].last_ms > 180000UL) {
            for (int j = i; j < g_model.ndets - 1; j++) g_model.dets[j] = g_model.dets[j + 1];
            g_model.ndets--;
            g_model.dets[g_model.ndets] = Det();
        } else i++;
    }
}

void model_clear() {
    // Reset in place: a Model temporary (about 5 KB) would land on the 8 KB Arduino loop stack
    for (int i = 0; i < MODEL_MAX_DETS; i++) g_model.dets[i] = Det();
    for (int i = 0; i < MODEL_MAX_APS; i++) g_model.aps[i] = ApRow();
    for (int i = 0; i < MODEL_MAX_BLE; i++) g_model.ble[i] = BleRow();
    g_model.ndets = g_model.naps = g_model.nble = 0;
    g_model.ble_total = 0; g_model.ble_ms = 0;
    g_model.aps_ms = g_model.list_ms = 0;
    g_model.list_total = 0;
    model_focus_clear();
}
