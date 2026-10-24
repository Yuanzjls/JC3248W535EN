# JPEGDEC (lib_deps), with its ESP32-S3 SIMD routines. Its own CMakeLists.txt is for a different
# setup (requires "jpegdec" and "esp-dsp" components), so it isn't used.
include(${CMAKE_CURRENT_LIST_DIR}/../libdeps.cmake)
libdeps_dir(dir JPEGDEC)

file(GLOB srcs ${dir}/src/*.cpp ${dir}/src/*.S)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${dir}/src)
