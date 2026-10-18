#include "wifi_store.h"

#include <algorithm>
#include <string.h>
#include <utility>
#include <vector>

#include "platform_time.h"
#include "sd_text.h"
#include "serial_log.h"
#include "wifi_net.h"

/*
 * /wifi_known.txt on SD: "KCWF1", then two lines per network - SSID and password, each
 * hex-encoded (UTF-8 SSIDs and any password bytes survive). Most recently used first.
 */
static const char *k_file = "/wifi_known.txt";
static const char *k_magic = "KCWF1";
static const size_t k_max_entries = 16;

static std::vector<std::pair<std::string, std::string>> s_known;
static bool s_loaded = false;

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + c - 'A';
    }
    return -1;
}

static std::string hex_encode(const std::string &s) {
    static const char *h = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        o += h[c >> 4];
        o += h[c & 0x0F];
    }
    return o;
}

/** Empty on malformed input. */
static std::string hex_decode(const std::string &hex) {
    if (hex.size() % 2) {
        return std::string();
    }
    std::string o;
    o.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_val(hex[i]);
        const int lo = hex_val(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::string();
        }
        o += (char)((hi << 4) | lo);
    }
    return o;
}

static void load_from_sd(void) {
    s_known.clear();
    std::string content;
    if (!sd_read_text(k_file, &content)) {
        serial_log("[wifi_store] %s not found on SD", k_file);
        return;
    }
    std::vector<std::string> lines;
    for_each_line(content, [&](const std::string &l) { lines.push_back(str_trim(l)); });
    if (lines.empty() || lines[0] != k_magic) {
        serial_log("[wifi_store] %s bad header", k_file);
        return;
    }
    for (size_t i = 1; i < lines.size() && s_known.size() < k_max_entries; i += 2) {
        if (lines[i].empty()) {
            break;
        }
        const std::string ssid = hex_decode(lines[i]);
        const std::string pass = (i + 1 < lines.size()) ? hex_decode(lines[i + 1]) : std::string();
        if (!ssid.empty()) {
            s_known.push_back({ssid, pass});
        }
    }
}

static bool write_to_sd(void) {
    std::string text = std::string(k_magic) + "\n";
    for (const auto &p : s_known) {
        text += hex_encode(p.first) + "\n" + hex_encode(p.second) + "\n";
    }
    if (!sd_write_text(k_file, text)) {
        serial_log("[wifi_store] cannot write %s", k_file);
        return false;
    }
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

bool wifi_store_find_password(const char *ssid, std::string *out_pass) {
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

void wifi_store_save(const char *ssid, const char *password) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    std::vector<std::pair<std::string, std::string>> next;
    next.reserve(s_known.size() + 1);
    next.push_back({ssid, password});
    for (const auto &p : s_known) {
        if (next.size() >= k_max_entries) {
            break;
        }
        if (p.first != ssid) {
            next.push_back(p);
        }
    }
    s_known = std::move(next);
    (void)write_to_sd();
}

void wifi_store_promote(const char *ssid) {
    if (!s_loaded) {
        wifi_store_begin();
    }
    auto it = std::find_if(s_known.begin(), s_known.end(), [&](const auto &p) { return p.first == ssid; });
    if (it == s_known.end()) {
        return;
    }
    const auto ent = *it;
    s_known.erase(it);
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
    s_ac_deadline = now_ms() + k_ac_timeout_ms;
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
    s_ac_scan_start = now_ms();
    if (!wifi_net_scan_start(false /*hidden*/, 120 /*ms/channel*/)) {
        serial_log("[wifi] scan failed, trying saved networks in order");
        build_candidates();
        s_ac_state = AcState::Settling;
        s_ac_deadline = now_ms() + k_ac_settle_ms;
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
        if (n == -1 && now_ms() - s_ac_scan_start < k_ac_scan_timeout_ms) {
            return true;
        }
        serial_log("[wifi] scan done in %lu ms, %d AP(s)", (unsigned long)(now_ms() - s_ac_scan_start), n);
        build_candidates();
        begin_candidate();
        return true;
    }

    case AcState::Connecting:
        if (wifi_net_connected()) {
            s_ac_state = AcState::Idle;
            return false;
        }
        if ((int32_t)(now_ms() - s_ac_deadline) < 0) {
            return true;
        }
        serial_log("[wifi] no luck after %lu s", (unsigned long)(k_ac_timeout_ms / 1000));
        wifi_net_disconnect();
        if (++s_ac_i >= s_ac_list.size()) {
            s_ac_state = AcState::Idle;
            return false;
        }
        s_ac_state = AcState::Settling;
        s_ac_deadline = now_ms() + k_ac_settle_ms;
        return true;

    case AcState::Settling:
        if ((int32_t)(now_ms() - s_ac_deadline) < 0) {
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
