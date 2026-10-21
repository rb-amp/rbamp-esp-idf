# Standalone IDF project for the rbAmp `deep_sleep_logger` example.
#
# Build/flash with:
#   cd examples/deep_sleep_logger
#   idf.py set-target esp32      # (or esp32s3, esp32c3, ...)
#   idf.py build flash monitor
#
# The component is two directories up (the repository root of a checkout, or
# components/rbamp in a project tree). IDF names a component after its
# directory, so a stub directory named "rbamp" that includes the component's
# CMake file is registered instead of the checkout directory itself.
cmake_minimum_required(VERSION 3.16)
get_filename_component(RBAMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(WRITE "${CMAKE_BINARY_DIR}/rbamp_component/rbamp/CMakeLists.txt"
     "include(\"${RBAMP_DIR}/CMakeLists.txt\")\n")
set(EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/rbamp_component")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rbamp_deep_sleep_logger)
