# ESP-IDF project (CMake), built through PlatformIO (framework = espidf). Configuration:
# sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16.0)

# Build only what the firmware uses: the project component (src/, PlatformIO's "main") and
# PlatformIO's helper component, plus whatever they require (src/CMakeLists.txt) and ESP-IDF's
# common core. Without this every ESP-IDF component is compiled (MQTT, OpenThread, USB host, ...).
# (ESP-IDF's MINIMAL_BUILD can't be used: it assumes the project component is called "main".)
set(COMPONENTS src __pio_env)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JC3248W535EN)
