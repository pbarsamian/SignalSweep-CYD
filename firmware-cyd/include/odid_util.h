// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Shared ASTM F3411 helpers (ported from upstream odidUseful/odidDecodeInto/
// applyDroneData). Used by both scanners.
#include "detection.h"
extern "C" {
#include "opendroneid.h"
}

static inline bool odid_useful(const ODID_UAS_Data& d) {
    return d.BasicIDValid[0] || d.LocationValid || d.SystemValid ||
           d.OperatorIDValid || d.SelfIDValid;
}

static inline void odid_decode_into(ODID_UAS_Data& out, const uint8_t* data, size_t len) {
    if (len == 0) return;
    if ((data[0] & 0xF0) == (ODID_MESSAGETYPE_PACKED << 4))
        odid_message_process_pack(&out, (uint8_t*)data, len);
    else
        decodeOpenDroneID(&out, (uint8_t*)data);
}

// Fields are only written when this frame carried them (Remote ID arrives as
// a stream of different message types).
static inline void odid_apply(Detection& d, const ODID_UAS_Data& u) {
    RemoteID& r = d.remote_id;
    bool any = false;
    if (u.BasicIDValid[0] && u.BasicID[0].UASID[0]) {
        snprintf(r.uas_id, sizeof(r.uas_id), "%s", u.BasicID[0].UASID);
        r.ua_type = (uint8_t)u.BasicID[0].UAType;
        any = true;
    }
    if (u.OperatorIDValid && u.OperatorID.OperatorId[0]) {
        snprintf(r.operator_id, sizeof(r.operator_id), "%s", u.OperatorID.OperatorId);
        any = true;
    }
    if (u.LocationValid) {
        r.loc_valid  = true;
        r.lat        = (float)u.Location.Latitude;
        r.lon        = (float)u.Location.Longitude;
        r.height_agl = u.Location.Height;
        r.alt_geo    = u.Location.AltitudeGeo;
        r.speed_ms   = u.Location.SpeedHorizontal;
        r.vspeed_ms  = u.Location.SpeedVertical;
        r.heading    = u.Location.Direction;
        any = true;
    }
    if (u.SystemValid && (u.System.OperatorLatitude != 0 || u.System.OperatorLongitude != 0)) {
        r.op_valid = true;
        r.op_lat = (float)u.System.OperatorLatitude;
        r.op_lon = (float)u.System.OperatorLongitude;
        any = true;
    }
    if (any) d.has_remote_id = true;
}
