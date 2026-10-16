// SignalSweep CYD v3.21
// Upstream: https://github.com/B0z0dcl0wn/SignalSweep (GPL-3.0)
// Board: ESP32-2432S028R "Cheap Yellow Display"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>

#include "board_pins.h"
#include "detection.h"
#include "settings.h"
#include "ui.h"
#include "scanner_ble.h"
#include "scanner_wifi.h"
#include "signatures.h"
#include "focus.h"

// ── Shared globals ────────────────────────────────────────────────────────────
DetectionRegistry g_reg;
SemaphoreHandle_t g_reg_mutex;
QueueHandle_t     g_queue;
Settings          g_cfg;

// Double-buffered scope: scan writes to [g_scope_w^1], UI reads from [g_scope_w]
uint8_t           g_scope[2][320] = {};
volatile int      g_scope_w = 0;

// BLE slot state — used by scanner_ble.cpp to boost scope peaks
uint8_t*          g_ble_scope_buf   = nullptr;
volatile bool     g_ble_slot_active = false;

static const int BLE_ADV_BUF_IDX[] = { 0, 191, 319 };

#define BLE_SLOT_MS   6000
#define WIFI_SLOT_MS  6000

// ── Scan task (Core 0) ────────────────────────────────────────────────────────
// Radio switching helpers (scan task only)
static bool s_wifi_ready = false;

static void radio_to_wifi() {
    NimBLEDevice::getScan()->stop();
    g_ble_slot_active = false;
    memset(g_scope[0], 0, 320);
    memset(g_scope[1], 0, 320);
    if (!s_wifi_ready) { scanner_wifi_init(g_cfg); s_wifi_ready = true; }
    esp_wifi_set_promiscuous(true);
}

static void radio_to_ble(bool full_duty) {
    if (s_wifi_ready) esp_wifi_set_promiscuous(false);
    memset(g_scope[0], 0, 320);
    memset(g_scope[1], 0, 320);
    g_ble_slot_active = true;
    scanner_ble_full_duty(full_duty);   // (re)starts the scan with the right window
}

static void scan_task(void*) {
    scanner_ble_init(g_cfg);
    vTaskDelay(pdMS_TO_TICKS(200));

    uint32_t slot_start = millis();
    bool ble_active   = true;
    uint8_t focus_mode = FOCUS_NONE;   // what the radio is currently doing for focus

    g_ble_scope_buf   = g_scope[0];
    g_ble_slot_active = true;

    for (;;) {
        uint32_t now = millis();
        uint8_t want = g_focus.active ? g_focus.radio : FOCUS_NONE;

        if (want != focus_mode) {
            // ── Enter / change / leave focus ─────────────────────────────────
            if (focus_mode == FOCUS_WIFI && s_wifi_ready) scanner_wifi_data_filter(false);
            if (want == FOCUS_WIFI) {
                if (ble_active) radio_to_wifi();
                ble_active = false;
                scanner_wifi_data_filter(true);   // peers/traffic need data frames
            }
            else if (want == FOCUS_BLE)  { radio_to_ble(true); ble_active = true; }
            else {
                // focus ended: resume the sweep, BLE slot first
                if (!ble_active) { radio_to_ble(false); ble_active = true; }
                else scanner_ble_full_duty(false);
            }
            focus_mode = want;
            slot_start = now;
        } else if (focus_mode == FOCUS_NONE) {
            // ── Normal sweep: alternate 6 s BLE / 6 s WiFi ───────────────────
            uint32_t slot_ms = ble_active ? BLE_SLOT_MS : WIFI_SLOT_MS;
            if (now - slot_start >= slot_ms) {
                if (ble_active) radio_to_wifi(); else radio_to_ble(false);
                ble_active = !ble_active;
                slot_start = now;
            }
        }

        // ── Per-loop scope update ─────────────────────────────────────────────
        if (!ble_active) {
            int lock = (focus_mode == FOCUS_WIFI) ? g_focus.channel : 0;
            scanner_wifi_tick(g_scope[g_scope_w ^ 1], lock);
            if (xSemaphoreTake(g_reg_mutex, 0) == pdTRUE) {
                ui_update_scope(g_scope[g_scope_w ^ 1], 320);
                xSemaphoreGive(g_reg_mutex);
            }
            g_scope_w ^= 1;
        } else {
            uint8_t* buf = g_scope[g_scope_w ^ 1];
            g_ble_scope_buf = buf;
            for (int i = 0; i < 320; i++)
                buf[i] = buf[i] > 2 ? buf[i] - 2 : 0;
            for (int idx : BLE_ADV_BUF_IDX)
                if (buf[idx] < 15) buf[idx] = 15;
            if (xSemaphoreTake(g_reg_mutex, 0) == pdTRUE) {
                ui_update_scope(buf, 320);
                xSemaphoreGive(g_reg_mutex);
            }
            g_scope_w ^= 1;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            // Focused WiFi target heard transmitting: keep its entry alive and
            // its RSSI current (matters for pivoted peers, which never re-match a rule)
            if (g_focus.active && g_focus.radio == FOCUS_WIFI && g_focus.last_tx_ms &&
                millis() - g_focus.last_tx_ms < 5000) {
                for (int i = 0; i < g_reg.count; i++)
                    if (memcmp(g_reg.items[i].mac, g_focus.mac, 6) == 0) {
                        g_reg.items[i].timestamp_ms = g_focus.last_tx_ms;
                        g_reg.items[i].rssi = g_focus.last_rssi;
                        break;
                    }
            }
            g_reg.prune(millis());
            xSemaphoreGive(g_reg_mutex);
        }
        static uint32_t s_stats_ms = 0;
        if (millis() - s_stats_ms >= 5000) {
            s_stats_ms = millis();
            if (focus_mode == FOCUS_NONE) Serial.printf("[slot] %s\n", ble_active ? "BLE" : "WiFi");
            else Serial.printf("[focus] %s ch%d, target frames=%u adverts=%u data=%u peers=%u\n",
                     focus_mode == FOCUS_WIFI ? "WiFi" : "BLE", g_focus.channel,
                     (unsigned)g_focus.frames, (unsigned)g_focus.adv_count,
                     (unsigned)g_focus.data_frames, (unsigned)g_focus.peer_count);
            stats_print();
        }
    }
}

// ── UI task (Core 1) ──────────────────────────────────────────────────────────
static void ui_task(void*) {
    ui_init(g_cfg);
    for (;;) {
        static DetectionRegistry snap;   // ~12 KB: keep it off the task stack
        if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            snap = g_reg;
            xSemaphoreGive(g_reg_mutex);
        }
        ui_update(snap, g_cfg);

        int idx;
        while (xQueueReceive(g_queue, &idx, 0) == pdTRUE) {
            if (xSemaphoreTake(g_reg_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (idx >= 0 && idx < g_reg.count)
                    ui_alert(g_reg.items[idx], g_cfg);
                xSemaphoreGive(g_reg_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33)); // ~30fps
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("[SignalSweep CYD v3.21] boot");

    // LED — all off (active LOW)
    pinMode(PIN_LED_R, OUTPUT); digitalWrite(PIN_LED_R, HIGH);
    pinMode(PIN_LED_G, OUTPUT); digitalWrite(PIN_LED_G, HIGH);
    pinMode(PIN_LED_B, OUTPUT); digitalWrite(PIN_LED_B, HIGH);

    settings_load(g_cfg);
    focus_init();

    g_reg_mutex = xSemaphoreCreateMutex();
    g_queue     = xQueueCreate(16, sizeof(int));

    xTaskCreatePinnedToCore(scan_task, "scan", 8192, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(ui_task,   "ui",   12288, nullptr, 1, nullptr, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
