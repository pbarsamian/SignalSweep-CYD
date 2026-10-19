// SPDX-License-Identifier: GPL-3.0-or-later
#include "battery.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "batt";

#if SS_BATT_SENSE
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_ch;
static bool s_ok, s_have_cali;
static uint16_t s_cached;
static int64_t s_cached_us;

void battery_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SS_BATT_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(SS_BATT_EN_GPIO, 0);           // divider off until we sample

    adc_oneshot_unit_init_cfg_t ic = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ic, &s_adc) != ESP_OK) { ESP_LOGW(TAG, "adc unit init failed"); return; }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(SS_BATT_ADC_GPIO, &unit, &s_ch) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC1 channel", SS_BATT_ADC_GPIO);
        return;
    }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_ch, &cc) != ESP_OK) { ESP_LOGW(TAG, "adc channel config failed"); return; }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .chan = s_ch,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    s_have_cali = adc_cali_create_scheme_curve_fitting(&cf, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t lf = { .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    s_have_cali = adc_cali_create_scheme_line_fitting(&lf, &s_cali) == ESP_OK;
#endif
    s_ok = true;
    ESP_LOGI(TAG, "sense on GPIO%d (enable GPIO%d), calibration %s", SS_BATT_ADC_GPIO,
             SS_BATT_EN_GPIO, s_have_cali ? "yes" : "no (linear 0-3300 mV)");
}

static uint16_t sample_mv(void) {
    gpio_set_level(SS_BATT_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));                // let the divider settle
    int sum = 0, got = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_ch, &raw) == ESP_OK) { sum += raw; got++; }
    }
    gpio_set_level(SS_BATT_EN_GPIO, 0);
    if (!got) return 0;
    int raw = sum / got, mv_pin = 0;
    if (!(s_have_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv_pin) == ESP_OK))
        mv_pin = raw * 3300 / 4095;               // wiki: effective range 0-3300 mV
    uint16_t vbat = (uint16_t)(mv_pin * 2);       // 2 x 100K divider
    return vbat < SS_BATT_MIN_VALID_MV ? 0 : vbat; // no cell / dead cell -> unavailable
}

uint16_t battery_mv(void) {
    if (!s_ok) return 0;
    int64_t now = esp_timer_get_time();
    if (s_cached_us == 0 || now - s_cached_us > 5000000) {
        s_cached = sample_mv();
        s_cached_us = now ? now : 1;
    }
    return s_cached;
}
#else
void battery_init(void) {}
uint16_t battery_mv(void) { return 0; }
#endif
