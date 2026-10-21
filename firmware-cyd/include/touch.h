#pragma once
#include <SPI.h>
#include <Arduino.h>
#include "board_pins.h"

// XPT2046 touch driver — self-contained, no external library needed.
// Calibration confirmed from working CYD projects.
// Raw X → screen Y (inverted), Raw Y → screen X

// The touch chip is bit-banged on its own four pins instead of using the VSPI controller. The microSD slot
// (issue #10) needs VSPI to itself on GPIO 18/19/23, and two devices with different pins on one controller
// means re-attaching the pins on every switch. A touch read is a few dozen clocks, so bit-banging costs
// nothing noticeable. SPI mode 0: data out on the falling edge, sampled on the rising edge.
static void touch_init() {
    pinMode(PIN_TOUCH_CLK,  OUTPUT); digitalWrite(PIN_TOUCH_CLK, LOW);
    pinMode(PIN_TOUCH_MOSI, OUTPUT); digitalWrite(PIN_TOUCH_MOSI, LOW);
    pinMode(PIN_TOUCH_MISO, INPUT);
    pinMode(PIN_TOUCH_CS,  OUTPUT); digitalWrite(PIN_TOUCH_CS, HIGH);
    pinMode(PIN_TOUCH_IRQ, INPUT);
}

static uint16_t _touch_bb_bits(uint16_t out, int nbits) {         // shift nbits out MSB first, return nbits read back
    uint16_t in = 0;
    for (int i = nbits - 1; i >= 0; i--) {
        digitalWrite(PIN_TOUCH_MOSI, (out >> i) & 1);
        digitalWrite(PIN_TOUCH_CLK, HIGH);
        delayMicroseconds(1);
        in = (uint16_t)((in << 1) | (digitalRead(PIN_TOUCH_MISO) ? 1 : 0));
        digitalWrite(PIN_TOUCH_CLK, LOW);
        delayMicroseconds(1);
    }
    return in;
}

static uint16_t _touch_read_raw(uint8_t cmd) {
    digitalWrite(PIN_TOUCH_CS, LOW);
    _touch_bb_bits(cmd, 8);
    uint16_t val = _touch_bb_bits(0, 16) >> 3;                    // null bit + 12 data bits + 3 trailing zeros
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
