#include "wifi_store.h"

#include <SD.h>
#include <vector>
#include <WiFi.h>

#include "serial_log.h"

static const char *k_file = "/wifi_known.txt";
static const char *k_magic = "KCWF1";
static const size_t k_max_entries = 16;

static std::vector<std::pair<String, String>> s_known;
static bool s_loaded = false;

static uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(10 + c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(10 + c - 'A');
    }
    return 0xFF;
}

static String hex_encode_bytes(const String &s) {
    static const char *h = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)s.c_str();
    const size_t n = s.length();
    String o;
    o.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        o += h[p[i] >> 4];
        o += h[p[i] & 0x0F];
    }
    return o;
}

static String hex_decode_to_string(const String &hex) {
    if (hex.length() % 2) {
        return String();
    }
    const size_t n = hex.length() / 2;
    String o;
    o.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const uint8_t hi = hex_val(hex[i * 2]);
        const uint8_t lo = hex_val(hex[i * 2 + 1]);
        if (hi > 15 || lo > 15) {
            return String();
        }
        o += (char)((hi << 4) | lo);
    }
    return o;
}

static void trim_crlf(String &s) {
    s.trim();
    while (s.length() && (s.endsWith("\r") || s.endsWith("\n"))) {
        s.remove(s.length() - 1);
    }
}

static bool read_line_trim(File &f, String &out) {
    if (!f.available()) {
        return false;
    }
    out = f.readStringUntil('\n');
    trim_crlf(out);
    return true;
}

static void load_from_sd(void) {
    s_known.clear();
    File f = SD.open(k_file, FILE_READ);
    if (!f) {
        return;
    }
    String line;
    if (!read_line_trim(f, line) || line != k_magic) {
        f.close();
        return;
    }
    while (f.available()) {
        String ssid_hex;
        if (!read_line_trim(f, ssid_hex) || ssid_hex.length() == 0) {
            break;
        }
        String pass_hex;
        if (f.available()) {
            (void)read_line_trim(f, pass_hex);
        }
        const String ssid = hex_decode_to_string(ssid_hex);
        const String pass = hex_decode_to_string(pass_hex);
        if (ssid.length() == 0) {
            continue;
        }
        s_known.push_back({ssid, pass});
        if (s_known.size() >= k_max_entries) {
            break;
        }
    }
    f.close();
}

static bool write_to_sd(void) {
    File f = SD.open(k_file, FILE_WRITE);
    if (!f) {
        serial_log("[wifi_store] cannot write %s", k_file);
        return false;
    }
    f.println(k_magic);
    for (const auto &p : s_known) {
        f.println(hex_encode_bytes(p.first));
        f.println(hex_encode_bytes(p.second));
    }
    f.close();
    return true;
}

void wifi_store_begin(void) {
    if (s_loaded) {
        return;
    }
    load_from_sd();
    s_loaded = true;
}

bool wifi_store_has_entries(void) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    return !s_known.empty();
}

bool wifi_store_find_password(const String &ssid, String *out_pass) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    for (const auto &p : s_known) {
        if (p.first == ssid) {
            *out_pass = p.second;
            return true;
        }
    }
    return false;
}

void wifi_store_save(const String &ssid, const String &password) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    std::vector<std::pair<String, String>> next;
    next.reserve(s_known.size() + 1);
    next.push_back({ssid, password});
    for (const auto &p : s_known) {
        if (p.first != ssid) {
            next.push_back(p);
        }
        if (next.size() >= k_max_entries) {
            break;
        }
    }
    s_known = std::move(next);
    (void)write_to_sd();
}

void wifi_store_promote(const String &ssid) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    size_t idx = s_known.size();
    for (size_t i = 0; i < s_known.size(); i++) {
        if (s_known[i].first == ssid) {
            idx = i;
            break;
        }
    }
    if (idx >= s_known.size()) {
        return;
    }
    const auto ent = s_known[idx];
    s_known.erase(s_known.begin() + (ptrdiff_t)idx);
    s_known.insert(s_known.begin(), ent);
    (void)write_to_sd();
}

static int s_ac_state = 0;
static size_t s_ac_i = 0;
static uint32_t s_ac_deadline = 0;
static const uint32_t k_ac_timeout_ms = 12000;

bool wifi_store_autoconnect_start(void) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    if (s_known.empty()) {
        return false;
    }
    s_ac_state = 1;
    s_ac_i = 0;
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_known[0].first.c_str(), s_known[0].second.c_str());
    s_ac_deadline = millis() + k_ac_timeout_ms;
    return true;
}

bool wifi_store_autoconnect_poll(void) {
    if (s_ac_state == 0) {
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        s_ac_state = 0;
        return false;
    }
    if ((int32_t)(millis() - s_ac_deadline) < 0) {
        return true;
    }
    s_ac_i++;
    if (s_ac_i >= s_known.size()) {
        WiFi.disconnect(true);
        s_ac_state = 0;
        return false;
    }
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_known[s_ac_i].first.c_str(), s_known[s_ac_i].second.c_str());
    s_ac_deadline = millis() + k_ac_timeout_ms;
    return true;
}

bool wifi_store_autoconnect_active(void) {
    return s_ac_state != 0;
}
