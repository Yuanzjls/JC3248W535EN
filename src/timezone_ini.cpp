#include "timezone_ini.h"

#include <SD.h>

#include "serial_log.h"

static const char *k_path = "/timezone.ini";

bool timezone_ini_load(String *out_posix) {
    if (!out_posix) {
        return false;
    }
    File f = SD.open(k_path, FILE_READ);
    if (!f) {
        serial_log("[timezone_ini] %s not found on SD", k_path);
        return false;
    }
    const String content = f.readString();
    f.close();

    bool in_tz = false;
    int start = 0;
    while (start < (int)content.length()) {
        int nl = content.indexOf('\n', start);
        if (nl < 0) {
            nl = content.length();
        }
        String line = content.substring(start, nl);
        start = nl + 1;
        line.trim();
        if (line.length() == 0) {
            continue;
        }
        if (line.charAt(0) == ';') {
            continue;
        }
        if (line.startsWith("[timezone]")) {
            in_tz = true;
            continue;
        }
        if (line.charAt(0) == '[') {
            in_tz = false;
            continue;
        }
        if (in_tz && line.startsWith("posix=")) {
            *out_posix = line.substring(6);
            out_posix->trim();
            if (out_posix->length() > 0) {
                return true;
            }
        }
    }
    serial_log("[timezone_ini] %s has no [timezone] posix= entry (%u bytes)", k_path, (unsigned)content.length());
    return false;
}

bool timezone_ini_save(const char *posix) {
    if (!posix || !posix[0]) {
        return false;
    }
    File f = SD.open(k_path, FILE_WRITE);
    if (!f) {
        serial_log("[timezone_ini] cannot write %s", k_path);
        return false;
    }
    f.println("; Timezone (POSIX TZ). Separate from WiFi credentials file.");
    f.println("[timezone]");
    f.print("posix=");
    f.println(posix);
    f.close();

    String check;
    const bool ok = timezone_ini_load(&check) && check == posix;
    serial_log("[timezone_ini] saved %s (read-back %s)", k_path, ok ? "OK" : "FAILED");
    return ok;
}
