// SPDX-License-Identifier: GPL-3.0-or-later
// theme.h — colours and layout shared by the UI pages. The palette and proportions are the
// standalone firmware's (dark oscilloscope look), so the split build feels the same.
#pragma once
#include <TFT_eSPI.h>
#include "board_pins.h"

#define HDR_H    16
#define TAB_H    34
#define PAGE_Y   HDR_H
#define PAGE_H   (SCREEN_H - HDR_H - TAB_H)
#define TAB_Y    (SCREEN_H - TAB_H)
#define TAB_W    80
#define ROW_H    25
#define ROWS_VIS (PAGE_H / ROW_H)

// Standard RGB565. Panel-level colour correction is done in setup (MADCTL 0xE8 + invert).
#define C_BLACK   TFT_BLACK
#define C_WHITE   TFT_WHITE
#define C_PANEL   0x1082
#define C_PANEL2  0x0841
#define C_BORDER  0x4208
#define C_GRID    0x2104
#define C_LTGREY  TFT_LIGHTGREY
#define C_MDGREY  0x7BCF
#define C_DKGREY  0x4208
#define C_SIGNAL  TFT_GREEN
#define C_SIGNAL2 0x0360
#define C_ACCENT  TFT_CYAN
#define C_DRONE   TFT_BLUE
#define C_TRACKER TFT_ORANGE
#define C_CAMERA  TFT_RED
#define C_BODYCAM TFT_MAGENTA
#define C_ATTACK  0xFD20
#define C_WARN    0xFD20
#define C_BAD     TFT_RED

// Threat types, same order as the C5's ThreatType and the "type" field on the wire
enum : uint8_t { T_DRONE = 0, T_TRACKER = 1, T_CAMERA = 2, T_BODYCAM = 3, T_ATTACK = 4, T_UNKNOWN = 5 };

static const uint16_t TCOL[]  = { C_DRONE, C_TRACKER, C_CAMERA, C_BODYCAM, C_ATTACK, C_DKGREY };
static const char*    TNAME[] = { "Drone", "Tracker", "Camera", "BodyCam", "Attack", "?" };
static const char*    TSHRT[] = { "DRN", "TRK", "CAM", "BCM", "ATK", "?" };

#define CONF_ALERT_MIN 70   // upstream: strongest single signal that raises an alert
