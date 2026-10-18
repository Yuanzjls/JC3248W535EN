#pragma once

#include <string>

/** Read [timezone] posix=... from /timezone.ini on SD. Returns false if missing/invalid. */
bool timezone_ini_load(std::string *out_posix);

/** Write POSIX TZ string to /timezone.ini (separate from wifi_known.txt). */
bool timezone_ini_save(const char *posix);
