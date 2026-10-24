# LVGL 8.3 (lib_deps), configured by include/lv_conf.h - not by LVGL's own ESP-IDF component and
# its Kconfig options.
include(${CMAKE_CURRENT_LIST_DIR}/../libdeps.cmake)
libdeps_dir(dir lvgl)

file(GLOB_RECURSE srcs ${dir}/src/*.c)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${dir}
                                    ${CMAKE_CURRENT_LIST_DIR}/../../include  # lv_conf.h, lv_mem_psram.h
                       REQUIRES esp_timer heap)

target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)
