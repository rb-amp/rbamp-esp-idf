# rbAmp ESP-IDF client component build descriptor.
#
# Drop the component into your project's components/ (as `rbamp`), add it
# as a git dependency, or install it from the ESP Component Registry.
# Targets IDF v5.2+ — uses the new `i2c_master` driver.
#
# Paths are given relative to this file rather than to the directory IDF
# registers the component from, so the component also builds through the
# stub directory the bundled examples create to expose it under the name
# "rbamp" whatever the checkout directory is called.
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/rbamp.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/rbamp_energy.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/rbamp_fleet.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/rbamp_std_decode.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/include"
    KCONFIG
        "${CMAKE_CURRENT_LIST_DIR}/Kconfig"
    REQUIRES
        esp_driver_i2c   # i2c_master_bus_handle_t, i2c_master_transmit_receive
        esp_driver_gpio  # DRDY line: input + pull-up + falling-edge ISR
        esp_timer        # esp_timer_get_time
        log              # ESP_LOG*
)
