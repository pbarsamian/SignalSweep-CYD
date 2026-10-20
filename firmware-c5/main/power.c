// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"
#include "esp_log.h"
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

// ---- state + side effects --------------------------------------------------------
static void set_state(ss_power_t next, uint16_t mv) {
    if (next == s_state) return;
    ESP_LOGW(TAG, "%s -> %s (battery %u mV)", name(s_state), name(next), (unsigned)mv);
    bool was_on = (s_state == SS_POWER_NORMAL), now_on = (next == SS_POWER_NORMAL);
    s_state = next;
    if (was_on != now_on) det_radio_enable(now_on);      // radio off in any charge state
}

void power_init(uint16_t boot_mv) {
    // Decided before the radio starts: a pack already at/below the threshold boots into
    // forced charge mode with the radio never switched on.
    s_state = power_boot_state(boot_mv);
    ESP_LOGI(TAG, "boot state %s (battery %u mV)", name(s_state), (unsigned)boot_mv);
}

ss_power_t power_state(void) { return s_state; }
bool power_radio_allowed(void) { return s_state == SS_POWER_NORMAL; }

void power_tick(uint16_t mv) { s_last_mv = mv; set_state(power_next_tick(s_state, mv), mv); }

ss_err_t power_request(bool charge) {
    ss_power_t next;
    ss_err_t e = power_next_request(s_state, charge, &next);
    if (e == SS_OK) set_state(next, s_last_mv);
    return e;
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
