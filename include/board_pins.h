#pragma once
// SignalSweep CYD — board pin definitions
// Board: ESP32-2432S028R "Cheap Yellow Display"
// All pins verified from prior project work on this board.

// Display — ILI9341 over HSPI
#define PIN_TFT_MISO  12
#define PIN_TFT_MOSI  13
#define PIN_TFT_SCLK  14
#define PIN_TFT_CS    15
#define PIN_TFT_DC     2
#define PIN_TFT_RST   -1
#define PIN_TFT_BL    21   // LEDC ch0 — backlight PWM

// Touch — XPT2046 over VSPI
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CLK  25
#define PIN_TOUCH_CS   33
#define PIN_TOUCH_IRQ  36

// RGB LED — active LOW
#define PIN_LED_R  4
#define PIN_LED_G 16
#define PIN_LED_B 17

// Speaker — SC8002B amp
#define PIN_SPEAKER 26

// LDR (light dependent resistor)
#define PIN_LDR 34   // input-only ADC

// MicroSD — VSPI (shared with touch SPI bus)
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23
#define PIN_SD_SCK  18
#define PIN_SD_CS    5

// Screen dimensions (landscape)
#define SCREEN_W 320
#define SCREEN_H 240

// Touch calibration — confirmed from working SD browser project
#define TOUCH_X_MIN  286
#define TOUCH_X_MAX 3823
#define TOUCH_Y_MIN  174
#define TOUCH_Y_MAX 3784

// Speaker tone frequencies (Hz)
#define TONE_DRONE   1200
#define TONE_TRACKER  800
#define TONE_CAMERA   600
#define TONE_BODYCAM  440
#define TONE_ATTACK  1800
#define TONE_MS       150
