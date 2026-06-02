idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES rbamp esp_driver_i2c esp_driver_gpio
)
