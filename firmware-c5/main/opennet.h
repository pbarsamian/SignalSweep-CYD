// SPDX-License-Identifier: GPL-3.0-or-later
// opennet.h — open-network profiling (issue #11). PASSIVE, headers and announcements only.
//
// While a Wi-Fi focus is active AND the owner has switched this on (default OFF, and it is switched
// off again on every new session), unencrypted data frames of the focused target are read just far
// enough to learn who is on the other end:
//   * addresses, IP protocol and TCP/UDP destination ports (header tallies; no payload),
//   * a device's own announcements: DHCP client hostname and vendor class, mDNS responses
//     (the names a device advertises for itself), SSDP NOTIFY SERVER.
// It never reads DNS queries or answers (port 53), never reads TCP payloads (so no TLS SNI, no HTTP),
// never reads mDNS/SSDP queries, and never touches a frame with the Protected bit set. Only the short
// extracted strings are kept, in RAM, for the life of the focus session. Nothing here transmits.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ON_MAX_PEERS 8
#define ON_HOST   24
#define ON_VC     20
#define ON_NAME   20
#define ON_NNAME  3
#define ON_SVC    20
#define ON_NSVC   2
#define ON_SSDP   28
#define ON_NPORT  4

struct OnPort { uint16_t port; uint8_t proto; uint16_t n; };      // proto: 6 TCP, 17 UDP

struct OnPeer {
    bool     used = false;
    uint8_t  mac[6] = {};
    char     host[ON_HOST] = {};                  // DHCP option 12
    char     vclass[ON_VC] = {};                  // DHCP option 60
    char     name[ON_NNAME][ON_NAME] = {};        // mDNS: names the device announces for itself
    char     svc[ON_NSVC][ON_SVC] = {};           // mDNS: service types it announces (_airplay._tcp)
    uint8_t  nnames = 0, nsvc = 0;
    char     ssdp[ON_SSDP] = {};                  // SSDP NOTIFY SERVER
    uint32_t tcp = 0, udp = 0, icmp = 0, other = 0;
    OnPort   ports[ON_NPORT];                     // destination ports this device sent to, busiest few
    uint32_t last_ms = 0;
};

void opennet_set_enabled(bool on);                // off also forgets everything
bool opennet_enabled(void);
void opennet_clear(void);
void opennet_note(const uint8_t* frame, int length);   // Wi-Fi task, DATA frames only; returns at once when off
int  opennet_snapshot(OnPeer* out, int max);           // busiest first; returns the count
