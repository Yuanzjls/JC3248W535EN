# ArduinoJson 7 (lib_deps): header-only.
include(${CMAKE_CURRENT_LIST_DIR}/../libdeps.cmake)
libdeps_dir(dir ArduinoJson)

idf_component_register(INCLUDE_DIRS ${dir}/src)
