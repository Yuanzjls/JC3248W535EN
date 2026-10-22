/*
 * minimp3 implementation (https://github.com/lieff/minimp3, CC0 / public domain; minimp3.h is the
 * unmodified upstream header). Its own translation unit so it can be built with -O2 while the rest
 * of the firmware uses -Os (src/CMakeLists.txt).
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
