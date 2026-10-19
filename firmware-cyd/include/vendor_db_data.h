// FALLBACK vendor data - a small built-in set so the firmware builds and runs
// before the full database is generated. Replace it by running:
//     python tools/gen_vendor_tables.py
// (needs internet once; see the script header for offline use)
#pragma once
#include <stdint.h>
#define VENDOR_DB_FULL 0

#define OUI24_COUNT 0
#define OUI28_COUNT 0
#define OUI36_COUNT 0
static const uint32_t OUI24_KEY[]  = {0};
static const uint32_t OUI24_NAME[] = {0};
static const uint32_t OUI28_KEY[]  = {0};
static const uint32_t OUI28_NAME[] = {0};
static const uint64_t OUI36_KEY[]  = {0};
static const uint32_t OUI36_NAME[] = {0};

// Bluetooth SIG company identifiers (sorted)
#define CID_COUNT 6
static const uint16_t CID_KEY[]  = { 0x0006, 0x004C, 0x0059, 0x0075, 0x00E0, 0x02E5 };
static const uint32_t CID_NAME[] = { 0, 10, 16, 37, 45, 52 };

// 16-bit service UUIDs (sorted)
#define UUID16_COUNT 19
static const uint16_t UUID16_KEY[] = {
    0x1800, 0x1801, 0x1802, 0x1803, 0x1804, 0x1805, 0x1809, 0x180A, 0x180D, 0x180F,
    0x1812, 0x1816, 0x1819, 0xFD59, 0xFD5A, 0xFEAA, 0xFEEC, 0xFEED, 0xFFFA };
static const uint32_t UUID16_NAME[] = {
    62, 77, 95, 111, 121, 130, 143, 162, 181, 192,
    200, 223, 249, 273, 273, 281, 288, 288, 293 };

static const char VENDOR_NAMES[] =
  "Microsoft\0"                  /*   0 */
  "Apple\0"                      /*  10 */
  "Nordic Semiconductor\0"       /*  16 */
  "Samsung\0"                    /*  37 */
  "Google\0"                     /*  45 */
  "Espressif\0"                  /*  52 */
  "Generic Access\0"             /*  62 */
  "Generic Attribute\0"          /*  77 */
  "Immediate Alert\0"            /*  95 */
  "Link Loss\0"                  /* 111 */
  "Tx Power\0"                   /* 121 */
  "Current Time\0"               /* 130 */
  "Health Thermometer\0"         /* 143 */
  "Device Information\0"         /* 162 */
  "Heart Rate\0"                 /* 181 */
  "Battery\0"                    /* 192 */
  "Human Interface Device\0"     /* 200 */
  "Cycling Speed and Cadence\0"  /* 223 */
  "Location and Navigation\0"    /* 249 */
  "Samsung\0"                    /* 273 */
  "Google\0"                     /* 281 */
  "Tile\0"                       /* 288 */
  "ASTM Remote ID\0"             /* 293 */
  ;
