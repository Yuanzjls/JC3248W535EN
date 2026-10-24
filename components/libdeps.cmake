# The libraries in platformio.ini lib_deps (LVGL, ArduinoJson, JPEGDEC, PNGdec), built as ESP-IDF
# components by ESP-IDF's own build (CMake + Ninja, scripts/idf_build.py). PlatformIO downloads
# them into .pio/libdeps/<env>/ before every `pio run` (or `pio pkg install`).
set(LIBDEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/../.pio/libdeps/JC3248W535EN")
get_filename_component(LIBDEPS_DIR "${LIBDEPS_DIR}" ABSOLUTE)

function(libdeps_dir var lib)
    if(NOT IS_DIRECTORY "${LIBDEPS_DIR}/${lib}")
        message(FATAL_ERROR "Library ${lib} not found in ${LIBDEPS_DIR} - run `pio pkg install`")
    endif()
    set(${var} "${LIBDEPS_DIR}/${lib}" PARENT_SCOPE)
endfunction()
