// SPDX-License-Identifier: GPL-3.0-or-later
// uart_slave.h — C5 side of the CYD link (UART1 on GPIO9 TX / GPIO10 RX).
#pragma once
#include <stdint.h>
#include <stddef.h>

// LOCKED pin map (BUILD_MANIFEST.md section 2): C5 TX = GPIO9 (pad D9) -> CYD RX (27);
// C5 RX = GPIO10 (pad D10) <- CYD TX (22).
#define SS_LINK_TX_GPIO 9
#define SS_LINK_RX_GPIO 10

void uart_slave_init(void);
void uart_slave_start(void);                  // spawns the rx/dispatch task
// Frame and transmit. Safe to call from the link task only (single writer).
void uart_slave_send(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t* payload, uint16_t len);
