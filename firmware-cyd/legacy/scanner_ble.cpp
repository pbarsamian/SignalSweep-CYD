// SignalSweep CYD — BLE scanner
// Detection logic ported from upstream B0z0dcl0wn/SignalSweep
// firmware/src/mode_watchers_watch.cpp (GPL-3.0-or-later):
//   - signature rules with upstream's AND semantics, weights and exact UUID match
//   - Apple Find My (long-form 0x12/0x19 only), Google FMDN, Tile, Samsung
//   - Flock solar battery ("Penguin")
//   - ASTM F3411 Remote ID over BLE (vendored opendroneid decoder)
// Scope boost on every advertisement is unchanged from v3.8.

#include <NimBLEDevice.h>
#include <Arduino.h>
#include "detection.h"
#include "settings.h"
#include "signatures.h"
#include "odid_util.h"
#include "focus.h"

extern Settings          g_cfg;
extern uint8_t*          g_ble_scope_buf;
extern volatile bool     g_ble_slot_active;

static const int BLE_ADV_BUF_IDX[] = { 0, 191, 319 };

// ~1 KB; file-static to keep it off the NimBLE task stack (upstream does the same)
static ODID_UAS_Data s_bleUas;

static void boost_ble_scope(int rssi) {
    if (!g_ble_slot_active || !g_ble_scope_buf) return;
    int str = constrain((rssi + 100) * 2, 20, 90);
    for (int idx : BLE_ADV_BUF_IDX) {
        for (int dx = -5; dx <= 5; dx++) {
            int xi = idx + dx;
            if (xi >= 0 && xi < 320) {
                int v = g_ble_scope_buf[xi] + str / (abs(dx) + 1);
                g_ble_scope_buf[xi] = (uint8_t)(v > 100 ? 100 : v);
            }
        }
    }
}

// ── Upstream uuidMatches(): exact match, never substring ────────────────────
static bool uuid_matches(const NimBLEUUID& u, const char* rule) {
    String want = String(rule);
    want.trim(); want.toLowerCase();
    if (want.startsWith("0x")) want = want.substring(2);
    if (want.length() == 4) {
        uint16_t v = (uint16_t)strtoul(want.c_str(), NULL, 16);
        if (u.bitSize() == 16) return u == NimBLEUUID(v);
        String full = String(u.toString().c_str()); full.toLowerCase();
        return full == ("0000" + want + "-0000-1000-8000-00805f9b34fb");
    }
    String have = String(u.toString().c_str()); have.toLowerCase();
    return have == want;
}

// ── Upstream matchDeviceAgainstRule() ───────────────────────────────────────
static int match_rule(NimBLEAdvertisedDevice* dev, const uint8_t mac[6], const Signature& s) {
    if (s.ssid[0]) return 0;                 // SSID rules are Wi-Fi only
    if (sig_gated_off(s, g_cfg)) return 0;   // attack gear while Attack is off
    int w = 0;
    if (s.oui[0]) {
        if (!oui_matches(mac, s.oui)) return 0;
        w += W_OUI;
    }
    if (s.mfg[0]) {
        if (!dev->haveManufacturerData()) return 0;
        std::string m = dev->getManufacturerData();
        if (m.length() < 2) return 0;
        uint16_t id = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
        if (id != parse_mfg_id(s.mfg)) return 0;
        w += W_MFG;
    }
    if (s.dev[0]) {
        if (!dev->haveName()) return 0;
        String n = String(dev->getName().c_str()); n.toLowerCase();
        String t = String(s.dev); t.toLowerCase();
        if (n.indexOf(t) < 0) return 0;
        w += W_NAME;
    }
    if (s.uuid[0]) {
        if (!dev->haveServiceUUID()) return 0;
        bool hit = false;
        size_t n = dev->getServiceUUIDCount();
        for (size_t i = 0; i < n && !hit; i++)
            hit = uuid_matches(dev->getServiceUUID(i), s.uuid);
        if (!hit) return 0;
        w += W_UUID;
    }
    if (w == 0) return 0;
    if (s.weight > 0) w = s.weight;
    return w > 100 ? 100 : w;
}

// ── Upstream bleIsAirtag(): long form only ──────────────────────────────────
// Every iPhone/iPad/Mac sends the 2-byte 0x12 status ping; only an item
// separated from its owner sends the 25-byte (0x19) offline-finding payload.
static bool ble_is_airtag(NimBLEAdvertisedDevice* dev) {
    if (!dev->haveManufacturerData()) return false;
    std::string m = dev->getManufacturerData();
    if (m.length() < 4) return false;
    uint16_t company = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
    return company == 0x004C && (uint8_t)m[2] == 0x12 && (uint8_t)m[3] == 0x19;
}

// ── Upstream bleTrackerNetwork(): FMDN / Tile / Samsung service data ───────
static const char* ble_tracker_network(NimBLEAdvertisedDevice* dev) {
    if (dev->getServiceDataCount() == 0) return nullptr;
    std::string fmdn = dev->getServiceData(NimBLEUUID((uint16_t)0xFEAA));
    if (fmdn.length() >= 1) {
        uint8_t frame = (uint8_t)fmdn[0];
        if (frame == 0x41) return "Google Find My (unwanted)";
        if (frame == 0x40) return "Google Find My Tracker";
        return nullptr;   // ordinary Eddystone beacon
    }
    if (dev->getServiceData(NimBLEUUID((uint16_t)0xFEED)).length() > 0) return "Tile Tracker";
    if (dev->getServiceData(NimBLEUUID((uint16_t)0xFD5A)).length() > 0) return "Samsung SmartTag";
    if (dev->getServiceData(NimBLEUUID((uint16_t)0xFD59)).length() > 0) return "Samsung SmartTag (setup)";
    return nullptr;
}

// ── Upstream blePenguin(): Flock solar battery ──────────────────────────────
static bool is_ten_digits(const std::string& s, size_t from) {
    if (s.length() != from + 10) return false;
    for (size_t i = from; i < s.length(); i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static bool ble_penguin(NimBLEAdvertisedDevice* dev) {
    std::string name = dev->haveName() ? dev->getName() : std::string();
    if (is_ten_digits(name, 8) && strncasecmp(name.c_str(), "penguin-", 8) == 0) return true;
    if (!dev->haveManufacturerData()) return false;
    std::string m = dev->getManufacturerData();
    if (m.length() < 2 || (uint8_t)m[0] != 0xC8 || (uint8_t)m[1] != 0x09) return false;
    if (is_ten_digits(name, 0)) return true;
    for (size_t i = 2; i + 2 < m.length(); i++)
        if (m[i] == 'T' && m[i+1] == 'N' && m[i+2] >= '0' && m[i+2] <= '9') return true;
    return false;
}

// ── Upstream bleDecodeRemoteId(): UUID 0xFFFA service data ──────────────────
static bool ble_decode_remote_id(NimBLEAdvertisedDevice* dev, ODID_UAS_Data& out) {
    uint8_t* p = dev->getPayload();
    size_t len = dev->getPayloadLength();
    if (!p || len < 4) return false;
    size_t off = 0;
    while (off + 1 < len) {
        uint8_t adLen = p[off];
        if (adLen == 0 || off + 1 + adLen > len) break;
        uint8_t adType = p[off + 1];
        if (adType == 0x16 && adLen >= 3) {
            uint16_t uuid = p[off + 2] | (p[off + 3] << 8);
            if (uuid == 0xFFFA) {
                // ASTM F3411 BT4 framing: UUID, app code 0x0D, counter, message
                memset(&out, 0, sizeof(out));
                if (adLen >= 5 + ODID_MESSAGE_SIZE && p[off + 4] == 0x0D)
                    odid_decode_into(out, &p[off + 6], adLen - 5);
                return true;   // presence alone = a drone broadcasting Remote ID
            }
        }
        off += adLen + 1;
    }
    return false;
}

// Parse the address from its string form so byte order is never in doubt.
static void parse_mac(NimBLEAdvertisedDevice* dev, uint8_t out[6]) {
    unsigned b[6] = {0};
    sscanf(dev->getAddress().toString().c_str(), "%x:%x:%x:%x:%x:%x",
           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
}

// ── Descriptive fields for the info view (no effect on scoring) ────────────
static uint16_t uuid16_of(const NimBLEUUID& u, bool& is128) {
    is128 = false;
    std::string t = u.toString();
    if (u.bitSize() == 16) return (uint16_t)strtoul(t.c_str() + (t.rfind("0x", 0) == 0 ? 2 : 0), NULL, 16);
    // 128-bit on the Bluetooth base UUID: 0000xxxx-0000-1000-8000-00805f9b34fb
    if (t.length() == 36 && t.compare(8, 28, "-0000-1000-8000-00805f9b34fb") == 0 &&
        t.compare(0, 4, "0000") == 0)
        return (uint16_t)strtoul(t.substr(4, 4).c_str(), NULL, 16);
    is128 = true;
    return 0;
}

static void add_svc(Detection& d, uint16_t v) {
    for (int k = 0; k < d.svc_count; k++) if (d.svc[k] == v) return;
    if (d.svc_count < MAX_SVC) d.svc[d.svc_count++] = v;
}

static void capture_fields(NimBLEAdvertisedDevice* a, Detection& d) {
    // Address type. Random sub-type from the two MSBs of the address.
    if (a->getAddress().getType() == BLE_ADDR_PUBLIC) {
        d.addr_type = ADDR_BLE_PUBLIC;
    } else {
        uint8_t top = d.mac[0] >> 6;
        d.addr_type = top == 3 ? ADDR_BLE_RANDOM_STATIC
                    : top == 1 ? ADDR_BLE_RPA
                    : top == 0 ? ADDR_BLE_NRPA : ADDR_UNKNOWN;
    }
    if (a->haveName()) snprintf(d.dev_name, sizeof(d.dev_name), "%s", a->getName().c_str());
    if (a->haveTXPower()) d.tx_power = a->getTXPower();
    if (a->haveAppearance()) { d.has_appearance = true; d.appearance = a->getAppearance(); }
    uint8_t at = a->getAdvType();
    // HCI LE advertising report event types: 0 ADV_IND, 1 ADV_DIRECT_IND are
    // connectable; 2 SCAN_IND, 3 NONCONN_IND, 4 SCAN_RSP are not.
    d.connectable = (at == 0 || at == 1);
    if (a->haveManufacturerData()) {
        std::string m = a->getManufacturerData();
        if (m.length() >= 2) {
            d.company = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
            if (d.company == 0x004C && m.length() >= 3) {
                d.apple_type = (uint8_t)m[2];
                if (d.apple_type == 0x12 && m.length() >= 4) d.apple_info = (uint8_t)m[3];
                else if (d.apple_type == 0x10 && m.length() >= 5) d.apple_info = (uint8_t)m[4];
            }
        }
    }
    bool is128;
    size_t n = a->getServiceUUIDCount();
    for (size_t i = 0; i < n; i++) {
        uint16_t v = uuid16_of(a->getServiceUUID(i), is128);
        if (is128) d.svc_128 = true; else add_svc(d, v);
    }
    n = a->getServiceDataCount();
    for (size_t i = 0; i < n; i++) {
        uint16_t v = uuid16_of(a->getServiceDataUUID(i), is128);
        if (is128) d.svc_128 = true; else add_svc(d, v);
    }
}

class SweepCallback : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* a) override {
        g_stats.ble_adv++;
        boost_ble_scope(a->getRSSI());

        uint8_t mac[6];
        parse_mac(a, mac);
        // Focus: deep-collect every advert from the target, matched or not
        if (focus_target(mac, FOCUS_BLE)) focus_note_ble(a->getPayload(), a->getPayloadLength());

        int confidence = 0, best = 0;
        const char* rule = nullptr;
        const char* category = nullptr;

        for (size_t i = 0; i < SIGNATURE_COUNT; i++) {
            int w = match_rule(a, mac, SIGNATURES[i]);
            if (w > 0) {
                confidence += w;
                if (w > best) { best = w; rule = SIGNATURES[i].name; category = SIGNATURES[i].category; }
            }
        }

        // Protocol detectors (upstream order): drone, AirTag, tracker net, Penguin
        bool drone = false, decoded = false;
        const char* net = ble_tracker_network(a);
        if (ble_decode_remote_id(a, s_bleUas)) {
            drone = true; decoded = odid_useful(s_bleUas);
            confidence += W_DRONE;
            if (W_DRONE > best) { best = W_DRONE; rule = "Remote ID Drone"; category = "Drone"; }
        } else if (ble_is_airtag(a)) {
            confidence += W_TRACKER;
            if (W_TRACKER > best) { best = W_TRACKER; rule = "Apple Find My Tracker"; category = "Tracker"; }
        } else if (net) {
            confidence += W_TRACKER;
            if (W_TRACKER > best) { best = W_TRACKER; rule = net; category = "Tracker"; }
        } else if (ble_penguin(a)) {
            confidence += W_PENGUIN;
            if (W_PENGUIN > best) { best = W_PENGUIN; rule = "Flock Battery (Penguin)"; category = "Flock Safety"; }
        }

        if (!rule) return;
        g_stats.ble_hit++;
        ThreatType t = threat_from_category(category);
        if (t == ThreatType::UNKNOWN || !type_enabled(t, g_cfg)) { g_stats.ble_drop++; return; }

        Detection d{};
        d.type         = t;
        d.timestamp_ms = millis();
        d.rssi         = a->getRSSI();
        d.channel      = 0;   // 0 = BLE (adv channel not reported by NimBLE)
        d.proto        = PROTO_BLE;
        memcpy(d.mac, mac, 6);
        snprintf(d.name, sizeof(d.name), "%s", rule);
        capture_fields(a, d);
        if (drone && decoded) odid_apply(d, s_bleUas);

        post_detection(d, confidence, best);
    }
};

static SweepCallback s_cb;

void scanner_ble_init(const Settings& s) {
    (void)s;   // live settings are read from g_cfg
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_N0);
    NimBLEScan* scan = NimBLEDevice::getScan();
    // Upstream: duplicates on (RSSI keeps updating), active scan (names arrive
    // in scan responses and the name rules need them), 100/50 interval/window,
    // and maxResults 0 so rotating random MACs are not retained forever.
    scan->setAdvertisedDeviceCallbacks(&s_cb, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(50);
    scan->setMaxResults(0);
    scan->start(0, nullptr, false);
    Serial.println("[ble] init OK");
}

// Focus on a BLE target: WiFi is off, so listen 100% of the time
void scanner_ble_full_duty(bool full) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setInterval(100);
    scan->setWindow(full ? 100 : 50);
    scan->start(0, nullptr, false);
}

void scanner_ble_tick() {
    vTaskDelay(pdMS_TO_TICKS(50));
}
