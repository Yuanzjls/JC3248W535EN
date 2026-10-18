# Main component: every source in src/, headers in include/.
# Libraries from platformio.ini lib_deps (LVGL, ArduinoJson, JPEGDEC, PNGdec) are built by
# PlatformIO and linked in.
# ESP-IDF components (esp-nimble-cpp, esp_new_jpeg) come from idf_component.yml.
file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "../include")
