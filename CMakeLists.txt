# ESP-IDF project (CMake + Ninja). `pio run` builds it with ESP-IDF's own build system
# (scripts/idf_build.py). Configuration: sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16.0)

# The generated full config keeps PlatformIO's name for it (sdkconfig.<env>).
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.JC3248W535EN")

# The project component is src/ (PlatformIO's layout), not main/. The lib_deps libraries are
# components in components/ (found automatically).
set(EXTRA_COMPONENT_DIRS src)

# Build only what the firmware uses: the project component plus whatever it requires
# (src/CMakeLists.txt) and ESP-IDF's common core. Without this every ESP-IDF component is compiled
# (MQTT, OpenThread, USB host, ...). (ESP-IDF's MINIMAL_BUILD can't be used: it assumes the
# project component is called "main".)
set(COMPONENTS src)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JC3248W535EN)
