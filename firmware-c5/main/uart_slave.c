// SPDX-License-Identifier: GPL-3.0-or-later
#include "uart_slave.h"
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ss_protocol.h"
#include "ss_frame.h"
#include "dispatch.h"

#define LINK_UART UART_NUM_1
static const char* TAG = "link";

static ss_parser_t s_parser;
static volatile uint32_t s_idle_ms = 20;
static uint8_t s_rx_payload[SS_MAX_PAYLOAD];
static uint8_t s_tx_frame[SS_MAX_PAYLOAD + SS_FRAME_OVERHEAD];

void uart_slave_init(void) {
    const uart_config_t cfg = {
        .baud_rate = SS_UART_BAUD_BRINGUP,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 8192, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, SS_LINK_TX_GPIO, SS_LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ss_parser_init(&s_parser, s_rx_payload);
    ESP_LOGI(TAG, "UART1 TX=GPIO%d RX=GPIO%d @%u", SS_LINK_TX_GPIO, SS_LINK_RX_GPIO,
             (unsigned)SS_UART_BAUD_BRINGUP);
}

void uart_slave_send(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t* payload, uint16_t len) {
    size_t n = ss_frame_encode(s_tx_frame, sizeof(s_tx_frame), type, seq, flags, payload, len);
    if (n) uart_write_bytes(LINK_UART, s_tx_frame, n);
}

static void link_task(void* arg) {
    (void)arg;
    uint8_t chunk[128];
    ss_frame_t f;
    for (;;) {
        int n = uart_read_bytes(LINK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(s_idle_ms));
        for (int i = 0; i < n; i++) {
            ss_parse_result_t r = ss_parser_feed(&s_parser, chunk[i], &f);
            if (r == SS_PARSE_FRAME) {
                dispatch_handle(&f);
            } else if (r == SS_PARSE_ERROR) {
                // Transport-level rejection: NACK carrying the error, echoing SEQ if known.
                uint8_t err = (uint8_t)f.err;
                uart_slave_send(SS_T_NACK, f.seq_valid ? f.seq : 0, 0, &err, 1);
                ESP_LOGW(TAG, "bad frame -> NACK err=%u", err);
            }
        }
        dispatch_tick();
    }
}

void uart_slave_set_idle_ms(uint32_t ms) { s_idle_ms = ms; }

void uart_slave_start(void) {
    xTaskCreate(link_task, "link", 6144, NULL, 5, NULL);
}
