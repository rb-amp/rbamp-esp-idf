# rbAmp ESP-IDF component build descriptor.
#
# Drop the component anywhere on your EXTRA_COMPONENT_DIRS, or install via
# `idf.py add-dependency "rbamp/rbamp^1.2"` once published to the ESP
# Component Registry. Targets IDF v5.2+ — uses the new `i2c_master` driver.

idf_component_register(
    SRCS
        "src/rbamp.c"
        "src/rbamp_energy.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "include"        # rbamp_energy.h is internal but co-located
    REQUIRES
        esp_driver_i2c   # i2c_master_bus_handle_t, i2c_master_transmit_receive
        esp_timer        # esp_timer_get_time
        log              # ESP_LOG*
)
