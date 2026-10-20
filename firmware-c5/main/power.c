// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "detections_api.h"

static const char* TAG = "power";
static ss_power_t s_state = SS_POWER_NORMAL;
static uint16_t s_last_mv = 0;                 // most recent reading, for log lines

static const char* name(ss_power_t s) {
    return s == SS_POWER_CHARGE_FORCED ? "CHARGE_FORCED" : s == SS_POWER_CHARGE_USER ? "CHARGE_USER" : "NORMAL";
}

// ---- the decision logic, as pure functions (exercised by power_selftest) ---------
ss_power_t power_boot_state(uint16_t mv) {
    return (mv && mv <= SS_BATT_LOW_MV) ? SS_POWER_CHARGE_FORCED : SS_POWER_NORMAL;
}

ss_power_t power_next_tick(ss_power_t s, uint16_t mv) {
    if (s == SS_POWER_CHARGE_FORCED)
        return mv > SS_BATT_EXIT_MV ? SS_POWER_NORMAL : SS_POWER_CHARGE_FORCED;   // the only way out
    if (mv && mv <= SS_BATT_LOW_MV) return SS_POWER_CHARGE_FORCED;                // from NORMAL or USER
    return s;
}

ss_err_t power_next_request(ss_power_t s, bool charge, ss_power_t* out) {
    if (s == SS_POWER_CHARGE_FORCED) { *out = s; return SS_ERR_LOCKED; }   // no leaving (or re-entering) while forced
    *out = charge ? SS_POWER_CHARGE_USER : SS_POWER_NORMAL;
    return SS_OK;
}

// ---- power savings while charging ---------------------------------------------------
//
// 1. CPU clock: 80 MHz in any charge state, 240 MHz otherwise (needs CONFIG_PM_ENABLE).
// 2. Optional CYD supply cut: SS_BOOST_EN_GPIO (-1 = not wired, the default) is driven
//    open-drain LOW after SS_CYD_CUT_DELAY_MS in a charge state, which switches off the
//    boost converter that feeds the CYD. It is released (EN floats high through the
//    external pull-up) the moment charge mode ends. EN MUST have a pull-up so the CYD
//    stays powered whenever the C5 is not actively cutting it.
// 3. BOOT button (GPIO28, active low): leaves a USER charge mode. It is the only way to
//    wake the system when the CYD, and so its touchscreen, is powered off. Ignored while
//    CHARGE_FORCED (the 4.0 V hold has no override).
#ifndef SS_BOOST_EN_GPIO
#define SS_BOOST_EN_GPIO -1
#endif
#define SS_BOOT_BTN_GPIO 28

static int64_t s_charge_since_us = 0;
static bool    s_cyd_cut = false;
static volatile bool s_btn_pressed = false;

static void IRAM_ATTR btn_isr(void* arg) { (void)arg; s_btn_pressed = true; }

static void apply_cpu(bool charge) {
    esp_pm_config_t cfg = {
        .max_freq_mhz = charge ? 80 : 240,
        .min_freq_mhz = charge ? 80 : 240,
        .light_sleep_enable = false,
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e == ESP_OK) ESP_LOGI(TAG, "CPU clock %d MHz", cfg.max_freq_mhz);
    else ESP_LOGW(TAG, "esp_pm_configure(%d MHz): %s", cfg.max_freq_mhz, esp_err_to_name(e));
}

static void boost_cut(bool cut) {
#if SS_BOOST_EN_GPIO >= 0
    gpio_set_level(SS_BOOST_EN_GPIO, cut ? 0 : 1);       // open-drain: 0 pulls EN low, 1 releases it
    ESP_LOGW(TAG, "CYD supply %s (EN on GPIO%d)", cut ? "CUT" : "restored", SS_BOOST_EN_GPIO);
#else
    (void)cut;
#endif
    s_cyd_cut = cut;
}

static void power_hw_init(void) {
#if SS_BOOST_EN_GPIO >= 0
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << SS_BOOST_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_set_level(SS_BOOST_EN_GPIO, 1);                  // released before the pin becomes an output
    gpio_config(&en);
    gpio_set_level(SS_BOOST_EN_GPIO, 1);
#endif
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << SS_BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(SS_BOOT_BTN_GPIO, btn_isr, NULL);
}

uint32_t power_cyd_cut_in_s(void) {
#if SS_BOOST_EN_GPIO >= 0
    if (s_state == SS_POWER_NORMAL || s_cyd_cut) return 0;
    int64_t left_ms = (int64_t)SS_CYD_CUT_DELAY_MS - (esp_timer_get_time() - s_charge_since_us) / 1000;
    return left_ms > 0 ? (uint32_t)((left_ms + 999) / 1000) : 1;
#else
    return 0;
#endif
}

// ---- state + side effects --------------------------------------------------------
static void set_state(ss_power_t next, uint16_t mv) {
    if (next == s_state) return;
    ESP_LOGW(TAG, "%s -> %s (battery %u mV)", name(s_state), name(next), (unsigned)mv);
    bool was_on = (s_state == SS_POWER_NORMAL), now_on = (next == SS_POWER_NORMAL);
    s_state = next;
    if (was_on != now_on) {
        det_radio_enable(now_on);                         // radio off in any charge state
        apply_cpu(!now_on);
        s_charge_since_us = esp_timer_get_time();
        if (now_on && s_cyd_cut) boost_cut(false);        // charge mode over: power the CYD back up
    }
}

void power_init(uint16_t boot_mv) {
    power_hw_init();
    // Decided before the radio starts: a pack already at/below the threshold boots into
    // forced charge mode with the radio never switched on.
    s_state = power_boot_state(boot_mv);
    s_charge_since_us = esp_timer_get_time();
    if (s_state != SS_POWER_NORMAL) apply_cpu(true);
    ESP_LOGI(TAG, "boot state %s (battery %u mV)", name(s_state), (unsigned)boot_mv);
}

ss_power_t power_state(void) { return s_state; }
bool power_radio_allowed(void) { return s_state == SS_POWER_NORMAL; }

ss_err_t power_request(bool charge) {
    ss_power_t next;
    ss_err_t e = power_next_request(s_state, charge, &next);
    if (e == SS_OK) set_state(next, s_last_mv);
    return e;
}

void power_tick(uint16_t mv) {
    s_last_mv = mv;
    set_state(power_next_tick(s_state, mv), mv);

    // BOOT button: leaves a USER charge mode (never a forced one)
    if (s_btn_pressed) {
        s_btn_pressed = false;
        if (s_state == SS_POWER_CHARGE_USER) {
            ESP_LOGW(TAG, "BOOT button: leaving charge mode");
            power_request(false);
        } else if (s_state == SS_POWER_CHARGE_FORCED) {
            ESP_LOGW(TAG, "BOOT button ignored: forced charge mode holds until the battery exceeds %u mV",
                     (unsigned)SS_BATT_EXIT_MV);
        }
    }

#if SS_BOOST_EN_GPIO >= 0
    // After the grace period in a charge state, cut the CYD's supply
    if (s_state != SS_POWER_NORMAL && !s_cyd_cut &&
        (esp_timer_get_time() - s_charge_since_us) / 1000 >= (int64_t)SS_CYD_CUT_DELAY_MS)
        boost_cut(true);
#endif
}

// ---- boot-time self-test of the decision logic ------------------------------------
bool power_selftest(void) {
    int fail = 0;
#define CHECK(label, cond) do { bool ok_ = (cond); if (!ok_) fail++; \
        ESP_LOGI(TAG, "selftest %-44s %s", label, ok_ ? "PASS" : "FAIL"); } while (0)
    ss_power_t o;
    CHECK("boot at 3300 mV -> forced",          power_boot_state(3300) == SS_POWER_CHARGE_FORCED);
    CHECK("boot at 3400 mV (<= low) -> forced", power_boot_state(3400) == SS_POWER_CHARGE_FORCED);
    CHECK("boot at 3401 mV -> normal",          power_boot_state(3401) == SS_POWER_NORMAL);
    CHECK("boot with no cell (0) -> normal",    power_boot_state(0) == SS_POWER_NORMAL);
    CHECK("normal, 3400 mV -> forced",          power_next_tick(SS_POWER_NORMAL, 3400) == SS_POWER_CHARGE_FORCED);
    CHECK("normal, 3401 mV -> stays normal",    power_next_tick(SS_POWER_NORMAL, 3401) == SS_POWER_NORMAL);
    CHECK("normal, reading 0 -> never forces",  power_next_tick(SS_POWER_NORMAL, 0) == SS_POWER_NORMAL);
    CHECK("USER charge, 3399 mV -> forced",     power_next_tick(SS_POWER_CHARGE_USER, 3399) == SS_POWER_CHARGE_FORCED);
    CHECK("USER charge, 3900 mV -> stays user", power_next_tick(SS_POWER_CHARGE_USER, 3900) == SS_POWER_CHARGE_USER);
    CHECK("forced, 3700 mV -> still forced",    power_next_tick(SS_POWER_CHARGE_FORCED, 3700) == SS_POWER_CHARGE_FORCED);
    CHECK("forced, 4000 mV -> still forced",    power_next_tick(SS_POWER_CHARGE_FORCED, 4000) == SS_POWER_CHARGE_FORCED);
    CHECK("forced, 4001 mV -> normal",          power_next_tick(SS_POWER_CHARGE_FORCED, 4001) == SS_POWER_NORMAL);
    CHECK("forced, reading 0 -> still forced",  power_next_tick(SS_POWER_CHARGE_FORCED, 0) == SS_POWER_CHARGE_FORCED);
    CHECK("request exit while forced -> LOCKED",power_next_request(SS_POWER_CHARGE_FORCED, false, &o) == SS_ERR_LOCKED
                                                && o == SS_POWER_CHARGE_FORCED);
    CHECK("request enter while forced -> LOCKED",power_next_request(SS_POWER_CHARGE_FORCED, true, &o) == SS_ERR_LOCKED);
    CHECK("normal + request -> USER",           power_next_request(SS_POWER_NORMAL, true, &o) == SS_OK && o == SS_POWER_CHARGE_USER);
    CHECK("USER can leave at any voltage",      power_next_request(SS_POWER_CHARGE_USER, false, &o) == SS_OK && o == SS_POWER_NORMAL);
#undef CHECK
    ESP_LOGI(TAG, "selftest %s (%d failed)", fail ? "FAILED" : "all passed", fail);
    return fail == 0;
}
