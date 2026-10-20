idf_component_register(
    SRCS "main.c" "uart_slave.c" "dispatch.c" "battery.c" "power.c" "tx_slots.c"
         "detections_api.cpp" "registry.cpp" "radio_wifi.cpp" "radio_ble.cpp" "signatures.cpp" "opendroneid.c" "odid_wifi.c" "odid_selftest.cpp"
    INCLUDE_DIRS "."
    REQUIRES shared driver esp_adc esp_timer esp_pm esp_wifi esp_event esp_netif nvs_flash bt)

# The vendored OpenDroneID core (Apache-2.0) must stay byte-for-byte unmodified. It prints uint32_t
# with %u, which this toolchain flags (uint32_t is long unsigned here): relax that one warning for
# those two files only instead of editing them.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/opendroneid.c" "${CMAKE_CURRENT_LIST_DIR}/odid_wifi.c"
                            PROPERTIES COMPILE_OPTIONS "-Wno-error=format;-Wno-format")
