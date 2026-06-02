# Standalone IDF project for the rbAmp `quick_read` example.
#
# Build/flash with:
#   cd examples/quick_read
#   idf.py set-target esp32      # (or esp32s3, esp32c3, ...)
#   idf.py build flash monitor
#
# EXTRA_COMPONENT_DIRS points back at libs/esp_idf/components/, where
# CMake finds the rbamp/ component automatically.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rbamp_quick_read)
