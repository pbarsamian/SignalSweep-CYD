// SPDX-License-Identifier: GPL-3.0-or-later
#include "odid_selftest.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "odid_util.h"
#include "registry.h"

// Test builds only: SS_RID_INJECT_TEST=1 posts the decoded self-test drone into the registry once,
// as if it had been heard over BLE, so the whole path (registry, event, alert, GET_DEVICE_PROFILE,
// the CYD's INFO page) can be exercised without a real drone. Default 0: nothing is injected.
#ifndef SS_RID_INJECT_TEST
#define SS_RID_INJECT_TEST 0
#endif

static const char* TAG = "rid";

bool odid_selftest(void) {
    int fail = 0;
#define CHECK(label, cond) do { bool ok_ = (cond); if (!ok_) fail++; \
        ESP_LOGI(TAG, "selftest %-40s %s", label, ok_ ? "PASS" : "FAIL"); } while (0)

    // A known drone broadcast: basic ID + location + operator ID + system (operator position)
    static ODID_UAS_Data tx;
    odid_initUasData(&tx);
    tx.BasicIDValid[0] = 1;
    tx.BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
    tx.BasicID[0].UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    snprintf(tx.BasicID[0].UASID, sizeof(tx.BasicID[0].UASID), "%s", "1596F1234567890ABCDE");
    tx.LocationValid = 1;
    tx.Location.Status = ODID_STATUS_AIRBORNE;
    tx.Location.Latitude = 40.123456;
    tx.Location.Longitude = -75.654321;
    tx.Location.AltitudeGeo = 123.5f;
    tx.Location.HeightType = ODID_HEIGHT_REF_OVER_GROUND;
    tx.Location.Height = 50.0f;
    tx.Location.SpeedHorizontal = 12.5f;
    tx.Location.SpeedVertical = -1.5f;
    tx.Location.Direction = 90.0f;
    tx.OperatorIDValid = 1;
    tx.OperatorID.OperatorIdType = ODID_OPERATOR_ID;
    snprintf(tx.OperatorID.OperatorId, sizeof(tx.OperatorID.OperatorId), "%s", "OP-TEST-0001");
    tx.SystemValid = 1;
    tx.System.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    tx.System.OperatorLatitude = 40.1;
    tx.System.OperatorLongitude = -75.6;

    uint8_t pack[ODID_PACK_MAX_MESSAGES * ODID_MESSAGE_SIZE + 8];
    memset(pack, 0, sizeof(pack));
    int n = odid_message_build_pack(&tx, pack, sizeof(pack));
    CHECK("message pack encodes", n > 0 && n <= (int)sizeof(pack));
    if (n <= 0) { ESP_LOGI(TAG, "selftest FAILED (%d failed)", fail + 0); return false; }

    // Decode exactly as the radios do: zeroed UAS data, then odid_decode_into, then odid_apply
    static ODID_UAS_Data rx;
    memset(&rx, 0, sizeof(rx));
    odid_decode_into(rx, pack, (size_t)n);
    CHECK("decoded data is useful", odid_useful(rx));

    static Detection d;
    d = Detection();
    odid_apply(d, rx);
    const RemoteID& r = d.remote_id;
    CHECK("detection flagged has_remote_id", d.has_remote_id);
    CHECK("UAS ID round-trips", strcmp(r.uas_id, "1596F1234567890ABCDE") == 0);
    CHECK("UA type = multirotor", r.ua_type == ODID_UATYPE_HELICOPTER_OR_MULTIROTOR);
    CHECK("operator ID round-trips", strcmp(r.operator_id, "OP-TEST-0001") == 0);
    CHECK("location valid", r.loc_valid);
    CHECK("latitude within 1e-4 deg", fabsf(r.lat - 40.123456f) < 1e-4f);
    CHECK("longitude within 1e-4 deg", fabsf(r.lon - (-75.654321f)) < 1e-4f);
    CHECK("height AGL ~50 m", fabsf(r.height_agl - 50.0f) < 1.0f);
    CHECK("altitude WGS84 ~123.5 m", fabsf(r.alt_geo - 123.5f) < 1.0f);
    CHECK("horizontal speed ~12.5 m/s", fabsf(r.speed_ms - 12.5f) < 0.5f);
    CHECK("heading ~90 deg", fabsf(r.heading - 90.0f) < 2.0f);
    CHECK("operator position valid", r.op_valid && fabsf(r.op_lat - 40.1f) < 1e-3f);

    // A single (unpacked) message goes through the other branch of odid_decode_into
    static ODID_BasicID_encoded enc;
    memset(&enc, 0, sizeof(enc));
    static ODID_UAS_Data one;
    memset(&one, 0, sizeof(one));
    int e = encodeBasicIDMessage(&enc, &tx.BasicID[0]);
    odid_decode_into(one, (const uint8_t*)&enc, sizeof(enc));
    CHECK("single message decodes", e == ODID_SUCCESS && one.BasicIDValid[0] &&
                                    strcmp(one.BasicID[0].UASID, "1596F1234567890ABCDE") == 0);
#undef CHECK
#if SS_RID_INJECT_TEST
    if (fail == 0) {
        static const uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
        memcpy(d.mac, mac, 6);
        d.type = ThreatType::DRONE;
        d.proto = PROTO_BLE;
        d.rssi = -58;
        d.channel = 0;
        d.band = 0;
        d.timestamp_ms = ms_now();
        d.addr_type = ADDR_BLE_RANDOM_STATIC;
        snprintf(d.name, sizeof(d.name), "%s", "Remote ID Drone (TEST)");
        post_detection(d, 90, 90);
        ESP_LOGW(TAG, "TEST BUILD: injected a synthetic Remote ID drone into the registry");
    }
#endif
    ESP_LOGI(TAG, "selftest %s (%d failed)", fail ? "FAILED" : "all passed", fail);
    return fail == 0;
}
