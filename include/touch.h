#pragma once
#include <SPI.h>
#include <Arduino.h>
#include "board_pins.h"

// XPT2046 touch driver — self-contained, no external library needed.
// Calibration confirmed from working CYD projects.
// Raw X → screen Y (inverted), Raw Y → screen X

static SPIClass touchSPI(VSPI);

static void touch_init() {
    touchSPI.begin(PIN_TOUCH_CLK, PIN_TOUCH_MISO, PIN_TOUCH_MOSI, PIN_TOUCH_CS);
    pinMode(PIN_TOUCH_CS,  OUTPUT); digitalWrite(PIN_TOUCH_CS, HIGH);
    pinMode(PIN_TOUCH_IRQ, INPUT);
}

static uint16_t _touch_read_raw(uint8_t cmd) {
    digitalWrite(PIN_TOUCH_CS, LOW);
    touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    touchSPI.transfer(cmd);
    uint16_t val = touchSPI.transfer16(0) >> 3;
    touchSPI.endTransaction();
    digitalWrite(PIN_TOUCH_CS, HIGH);
    return val;
}

// Returns true if screen is being touched, and sets tx/ty to screen coordinates.
// Landscape orientation: raw X→screen Y (inverted), raw Y→screen X (inverted)
static bool touch_get(int& tx, int& ty) {
    if (digitalRead(PIN_TOUCH_IRQ) != LOW) return false;

    int xs = 0, ys = 0;
    for (int i = 0; i < 3; i++) {
        xs += _touch_read_raw(0xD0); // X channel
        ys += _touch_read_raw(0x90); // Y channel
    }
    int rx = xs / 3;
    int ry = ys / 3;

    // Reject only obvious noise; clamp edge readings instead of dropping them
    if (rx < 100 || ry < 100) return false;

    // Both axes inverted — matches the original confirmed-working touch.h (v2.6)
    // raw X -> screen Y (inverted), raw Y -> screen X (inverted)
    ty = map(rx, TOUCH_X_MAX, TOUCH_X_MIN, 0, SCREEN_H - 1);
    tx = map(ry, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, SCREEN_W - 1);
    tx = constrain(tx, 0, SCREEN_W - 1);
    ty = constrain(ty, 0, SCREEN_H - 1);

    Serial.printf("[touch] raw=%d,%d -> tx=%d ty=%d\n", rx, ry, tx, ty);
    return true;
}
