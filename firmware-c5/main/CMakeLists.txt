idf_component_register(
    SRCS "main.c" "uart_slave.c" "dispatch.c" "battery.c" "tx_slots.c"
    INCLUDE_DIRS "."
    REQUIRES shared driver esp_adc esp_timer)
