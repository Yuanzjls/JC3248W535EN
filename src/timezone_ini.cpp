#include "timezone_ini.h"

#include "sd_text.h"
#include "serial_log.h"

static const char *k_path = "/timezone.ini";

bool timezone_ini_load(std::string *out_posix) {
    if (!out_posix) {
        return false;
    }
    std::string content;
    if (!sd_read_text(k_path, &content)) {
        serial_log("[timezone_ini] %s not found on SD", k_path);
        return false;
    }
    out_posix->clear();
    ini_parse(content, [&](const std::string &section, const std::string &key, const std::string &val) {
        if (out_posix->empty() && section == "timezone" && key == "posix") {
            *out_posix = val;
        }
    });
    if (!out_posix->empty()) {
        return true;
    }
    serial_log("[timezone_ini] %s has no [timezone] posix= entry (%u bytes)", k_path, (unsigned)content.size());
    return false;
}

bool timezone_ini_save(const char *posix) {
    if (!posix || !posix[0]) {
        return false;
    }
    const std::string text = std::string("; Timezone (POSIX TZ). Separate from WiFi credentials file.\n"
                                         "[timezone]\n"
                                         "posix=") +
                             posix + "\n";
    if (!sd_write_text(k_path, text)) {
        serial_log("[timezone_ini] cannot write %s", k_path);
        return false;
    }
    std::string check;
    const bool ok = timezone_ini_load(&check) && check == posix;
    serial_log("[timezone_ini] saved %s (read-back %s)", k_path, ok ? "OK" : "FAILED");
    return ok;
}
