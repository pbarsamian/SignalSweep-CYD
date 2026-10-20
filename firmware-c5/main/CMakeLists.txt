idf_component_register(
    SRCS "main.c" "uart_slave.c" "dispatch.c" "battery.c" "power.c" "tx_slots.c"
         "detections_api.cpp" "registry.cpp" "radio_wifi.cpp" "signatures.cpp"
    INCLUDE_DIRS "."
    REQUIRES shared driver esp_adc esp_timer esp_wifi esp_event esp_netif nvs_flash)
