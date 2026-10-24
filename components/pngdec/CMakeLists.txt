# PNGdec (lib_deps), with its bundled zlib and ESP32-S3 SIMD routine.
include(${CMAKE_CURRENT_LIST_DIR}/../libdeps.cmake)
libdeps_dir(dir PNGdec)

file(GLOB srcs ${dir}/src/*.c ${dir}/src/*.cpp ${dir}/src/*.S)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${dir}/src)

# PUBLIC: both change PNGdec.h, which the decoder and its users must see the same way.
# PNGdec's default line buffer caps PNG width at ~640 px (RGBA). Allow up to 4096 px wide; the
# decoder object lives in PSRAM (media_ui.cpp), so the bigger buffer is cheap.
# PNGdec.h takes its plain-C (non-Arduino) branch only for macOS/Linux/MCUXpresso, not for
# ESP_PLATFORM like JPEGDEC does; __MCUXPRESSO selects it. Nothing else checks this macro.
target_compile_definitions(${COMPONENT_LIB} PUBLIC PNG_MAX_BUFFERED_PIXELS=32770 __MCUXPRESSO)
