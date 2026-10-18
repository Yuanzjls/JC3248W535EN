#include "wifi_store.h"

#include "sd_card.h"
#include <algorithm>
#include <string.h>
#include <vector>
#include <Arduino.h>

#include "wifi_net.h"

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
    File f = sd_fs().open(k_file, FILE_READ);
    if (!f) {
        serial_log("[wifi_store] %s not found on SD", k_file);
        return;
    }
    String line;
    if (!read_line_trim(f, line) || line != k_magic) {
        serial_log("[wifi_store] %s bad header", k_file);
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
    File f = sd_fs().open(k_file, FILE_WRITE);
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
    serial_log("[wifi_store] saved %u network(s) to %s", (unsigned)s_known.size(), k_file);
    return true;
}

void wifi_store_begin(void) {
    if (s_loaded) {
        return;
    }
    load_from_sd();
    serial_log("[wifi_store] loaded %u network(s) from %s", (unsigned)s_known.size(), k_file);
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

/*
 * Auto-connect: scan first, then try the saved networks that are actually visible, strongest
 * first, targeting the scanned channel + BSSID. Saved networks not seen in the scan (out of
 * range, or hidden SSIDs) are tried last. Blindly trying them in saved order wasted ~12 s on
 * an unreachable network every boot.
 */
enum class AcState { Idle, Scanning, Connecting, Settling };

struct AcCandidate {
    size_t idx;     /* into s_known */
    int32_t rssi;   /* -127 if not seen in the scan */
    int32_t channel;
    uint8_t bssid[6];
    bool seen;
};

static AcState s_ac_state = AcState::Idle;
static std::vector<AcCandidate> s_ac_list;
static size_t s_ac_i = 0;
static uint32_t s_ac_deadline = 0;
static uint32_t s_ac_scan_start = 0;
static const uint32_t k_ac_timeout_ms = 12000; /* per network */
static const uint32_t k_ac_scan_timeout_ms = 6000;
/* After disconnect, let the driver leave "connecting" before the next begin(); begin() too soon
 * fails with "sta is connecting, cannot set config". */
static const uint32_t k_ac_settle_ms = 800;

static void build_candidates(void) {
    int n = 0;
    const wifi_ap_record_t *aps = wifi_net_scan_results(&n);
    s_ac_list.clear();
    for (size_t k = 0; k < s_known.size(); k++) {
        AcCandidate c = {k, -127, 0, {0}, false};
        for (int i = 0; i < n; i++) {
            if (strcmp((const char *)aps[i].ssid, s_known[k].first.c_str()) == 0 && aps[i].rssi > c.rssi) {
                c.rssi = aps[i].rssi;
                c.channel = aps[i].primary;
                memcpy(c.bssid, aps[i].bssid, sizeof(c.bssid));
                c.seen = true;
            }
        }
        s_ac_list.push_back(c);
    }
    /* Visible networks first, strongest first; unseen keep their saved order at the end. */
    std::stable_sort(s_ac_list.begin(), s_ac_list.end(), [](const AcCandidate &a, const AcCandidate &b) {
        if (a.seen != b.seen) {
            return a.seen;
        }
        return a.seen && a.rssi > b.rssi;
    });
    for (const auto &c : s_ac_list) {
        if (c.seen) {
            serial_log("[wifi]   saved \"%s\" visible: %d dBm, ch %d", s_known[c.idx].first.c_str(), (int)c.rssi,
                       (int)c.channel);
        } else {
            serial_log("[wifi]   saved \"%s\" not seen in scan", s_known[c.idx].first.c_str());
        }
    }
}

static void begin_candidate(void) {
    const AcCandidate &c = s_ac_list[s_ac_i];
    const auto &net = s_known[c.idx];
    serial_log("[wifi] trying saved \"%s\" (%u/%u)%s", net.first.c_str(), (unsigned)(s_ac_i + 1),
               (unsigned)s_ac_list.size(), c.seen ? "" : " - not in scan, trying anyway");
    (void)wifi_net_connect(net.first.c_str(), net.second.c_str(), c.seen ? c.channel : 0, c.seen ? c.bssid : nullptr);
    s_ac_state = AcState::Connecting;
    s_ac_deadline = millis() + k_ac_timeout_ms;
}

bool wifi_store_autoconnect_start(void) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    if (s_known.empty()) {
        return false;
    }
    wifi_net_disconnect(); /* association only: the radio stays on */
    s_ac_i = 0;
    s_ac_scan_start = millis();
    if (!wifi_net_scan_start(false /*hidden*/, 120 /*ms/channel*/)) {
        serial_log("[wifi] scan failed, trying saved networks in order");
        build_candidates();
        s_ac_state = AcState::Settling;
        s_ac_deadline = millis() + k_ac_settle_ms;
    } else {
        serial_log("[wifi] scanning for saved networks...");
        s_ac_state = AcState::Scanning;
    }
    return true;
}

bool wifi_store_autoconnect_poll(void) {
    switch (s_ac_state) {
    case AcState::Idle:
        return false;

    case AcState::Scanning: {
        const int n = wifi_net_scan_poll();
        if (n == -1 && millis() - s_ac_scan_start < k_ac_scan_timeout_ms) {
            return true;
        }
        serial_log("[wifi] scan done in %lu ms, %d AP(s)", (unsigned long)(millis() - s_ac_scan_start), n);
        build_candidates();
        begin_candidate();
        return true;
    }

    case AcState::Connecting:
        if (wifi_net_connected()) {
            s_ac_state = AcState::Idle;
            return false;
        }
        if ((int32_t)(millis() - s_ac_deadline) < 0) {
            return true;
        }
        serial_log("[wifi] no luck after %lu s", (unsigned long)(k_ac_timeout_ms / 1000));
        wifi_net_disconnect();
        if (++s_ac_i >= s_ac_list.size()) {
            s_ac_state = AcState::Idle;
            return false;
        }
        s_ac_state = AcState::Settling;
        s_ac_deadline = millis() + k_ac_settle_ms;
        return true;

    case AcState::Settling:
        if ((int32_t)(millis() - s_ac_deadline) < 0) {
            return true;
        }
        begin_candidate();
        return true;
    }
    return false;
}

bool wifi_store_autoconnect_active(void) {
    return s_ac_state != AcState::Idle;
}
