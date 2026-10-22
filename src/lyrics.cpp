#include "lyrics.h"

#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#include "platform_time.h"
#include "sd_card.h"
#include "sd_text.h"
#include "serial_log.h"
#include "wifi_net.h"

/*
 * LRCLIB API (https://lrclib.net/docs): GET /api/get?track_name&artist_name&duration matches
 * exactly (duration within 2 s); GET /api/search returns up to 20 records (full lyrics included,
 * ~250 KB of JSON for a popular title). HTTPS only; mbedTLS allocates in PSRAM
 * (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC), bodies and JSON are in PSRAM too - internal RAM is scarce.
 *
 * Lookup order, first hit wins: exact get (when an artist is known) -> search title + artist ->
 * search title -> free-text search. A search record counts only if its duration is within
 * k_max_dur_diff_s of the track's; synced lyrics beat plain ones, then the closest duration.
 */

static const char *k_api = "https://lrclib.net/api";
static const char *k_user_agent = "JC3248W535EN-music/1.0 (https://github.com/Yuanzjls/JC3248W535EN)";
static const char *k_miss_dir = "/.cache/lyrics";
static const float k_max_dur_diff_s = 3.0f;
static const size_t k_max_body = 1024 * 1024;

struct Request {
    std::string path, title, artist;
    float dur_s = 0;
    uint32_t id = 0;
    bool retry = false;
};

static SemaphoreHandle_t s_mtx = nullptr; /* s_req, s_req_new, s_done */
static TaskHandle_t s_task = nullptr;
static Request s_req;
static bool s_req_new = false;
static LyricsResult *s_done = nullptr;

/* Same as weather.cpp: ArduinoJson nodes and strings in PSRAM. */
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void deallocate(void *p) override {
        heap_caps_free(p);
    }
    void *reallocate(void *p, size_t n) override {
        return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};
static PsramJsonAllocator s_json_alloc;

/* ---------- helpers ---------- */

static bool superseded(void) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool n = s_req_new;
    xSemaphoreGive(s_mtx);
    return n;
}

static void publish(LyricsResult *r) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    delete s_done; /* never taken: superseded */
    s_done = r;
    xSemaphoreGive(s_mtx);
}

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) {
        b--;
    }
    return s.substr(a, b - a);
}

/** "/music/a.mp3" -> "/music/a.lrc" */
static std::string lrc_path(const std::string &mp3) {
    const size_t dot = mp3.rfind('.');
    return (dot == std::string::npos ? mp3 : mp3.substr(0, dot)) + ".lrc";
}

/** Marker file for "no lyrics found" (FNV-1a of the path: short, no folder structure needed). */
static std::string miss_path(const std::string &mp3) {
    uint32_t h = 2166136261u;
    for (unsigned char c : mp3) {
        h = (h ^ c) * 16777619u;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%s/%08lx.none", k_miss_dir, (unsigned long)h);
    return buf;
}

static bool card_exists(const std::string &path) {
    struct stat st;
    return stat(sd_path(path.c_str()).c_str(), &st) == 0;
}

static void mark_missing(const std::string &mp3) {
    (void)mkdir(sd_path("/.cache").c_str(), 0775);
    (void)mkdir(sd_path(k_miss_dir).c_str(), 0775);
    (void)sd_write_text(miss_path(mp3).c_str(), mp3 + "\n");
}

/* ---------- LRC ---------- */

/** "mm:ss.xx" / "mm:ss.xxx" / "mm:ss" -> ms, or -1. */
static int32_t parse_time(const char *s, size_t n) {
    unsigned mm = 0, ss = 0, frac = 0, digits = 0;
    size_t i = 0;
    while (i < n && isdigit((unsigned char)s[i])) {
        mm = mm * 10 + (s[i++] - '0');
    }
    if (i == 0 || i >= n || s[i++] != ':') {
        return -1;
    }
    const size_t ss0 = i;
    while (i < n && isdigit((unsigned char)s[i])) {
        ss = ss * 10 + (s[i++] - '0');
    }
    if (i == ss0) {
        return -1;
    }
    if (i < n && (s[i] == '.' || s[i] == ':')) {
        i++;
        while (i < n && isdigit((unsigned char)s[i]) && digits < 3) {
            frac = frac * 10 + (s[i++] - '0');
            digits++;
        }
        while (digits < 3) { /* .5 -> 500 ms, .25 -> 250 ms */
            frac *= 10;
            digits++;
        }
    }
    return i == n ? (int32_t)((mm * 60 + ss) * 1000 + frac) : -1;
}

/**
 * Enhanced LRC: "<mm:ss.xx>" before each word ("<00:12.34> Why <00:12.80> you ..."). Returns the
 * text without the markers (spaces collapsed, trimmed) and each word's start time and position in it.
 */
static std::string strip_words(const std::string &s, std::vector<LyricWord> *words) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            const size_t close = s.find('>', i);
            const int32_t t = close == std::string::npos ? -1 : parse_time(s.c_str() + i + 1, close - i - 1);
            if (t >= 0) {
                words->push_back({t, (uint32_t)out.size()});
                i = close + 1;
                continue;
            }
        }
        char c = s[i++];
        if (c == '\t') {
            c = ' ';
        }
        if (c != ' ' || (!out.empty() && out.back() != ' ')) { /* no leading or doubled spaces */
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    /* Drop markers that start no word (e.g. the end time after the last word). */
    words->erase(std::remove_if(words->begin(), words->end(), [&](const LyricWord &w) { return w.pos >= out.size(); }),
                 words->end());
    return out;
}

/** Parse LRC (or plain text) into r. Lines with [mm:ss.xx] tags make it Synced; [offset:] applies. */
static void parse_lrc(const std::string &src, LyricsResult *r) {
    struct Timed {
        int32_t ms;
        std::string text;
        std::vector<LyricWord> words;
    };
    std::vector<Timed> timed;
    std::vector<std::string> plain;
    int32_t offset_ms = 0;

    size_t pos = src.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; /* UTF-8 BOM */
    while (pos < src.size()) {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) {
            eol = src.size();
        }
        std::string line = src.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::vector<int32_t> times;
        bool tagged = false;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            const size_t close = line.find(']', i);
            if (close == std::string::npos) {
                break;
            }
            const char *tag = line.c_str() + i + 1;
            const size_t n = close - i - 1;
            const int32_t t = parse_time(tag, n);
            if (t >= 0) {
                times.push_back(t);
            } else if (n > 7 && strncasecmp(tag, "offset:", 7) == 0) {
                offset_ms = atoi(tag + 7);
            }
            tagged = true;
            i = close + 1;
        }
        std::vector<LyricWord> words;
        const std::string text = strip_words(line.substr(i), &words);
        if (times.size() > 1) {
            words.clear(); /* one text at several times: absolute word times can't fit all */
        }
        for (int32_t t : times) {
            timed.push_back({t, text, words});
        }
        if (!tagged && !text.empty()) {
            plain.push_back(text);
        }
    }

    r->lines.clear();
    r->words.clear();
    r->text.clear();
    if (!timed.empty()) {
        std::stable_sort(timed.begin(), timed.end(), [](const Timed &a, const Timed &b) { return a.ms < b.ms; });
        r->kind = LyricsKind::Synced;
        for (const Timed &t : timed) {
            /* A positive offset shows the lyrics earlier. */
            r->lines.push_back({std::max<int32_t>(0, t.ms - offset_ms), (uint32_t)r->text.size(),
                                (uint32_t)r->words.size(), (uint32_t)t.words.size()});
            for (const LyricWord &w : t.words) {
                r->words.push_back({std::max<int32_t>(0, w.ms - offset_ms), w.pos});
            }
            r->text.append(t.text).push_back('\0');
        }
    } else if (!plain.empty()) {
        r->kind = LyricsKind::Plain;
        for (const std::string &p : plain) {
            r->lines.push_back({-1, (uint32_t)r->text.size(), 0, 0});
            r->text.append(p).push_back('\0');
        }
    } else {
        r->kind = LyricsKind::None;
    }
}

/* ---------- LRCLIB ---------- */

/** One GET; returns the HTTP status (body in *body, PSRAM) or -1 on a network error. */
static int http_get_once(const std::string &url, std::string *body) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 15000;
    cfg.user_agent = k_user_agent;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    int code = -1;
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        serial_log("[lyrics] GET %s -> %s", url.c_str(), esp_err_to_name(err));
    } else {
        (void)esp_http_client_fetch_headers(client);
        code = esp_http_client_get_status_code(client);
        body->clear();
        char buf[1024];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && body->size() < k_max_body) {
            body->append(buf, n);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return code;
}

/**
 * GET with retries: LRCLIB often answers 503 "ServerOverloaded" for a moment (and 429 when rate
 * limited), so a network error, 429 or 5xx is retried after 2 s, then 4 s. Logs every non-200.
 */
static int http_get(const std::string &url, std::string *body) {
    static const int k_tries = 3;
    int code = -1;
    for (int attempt = 1; attempt <= k_tries; attempt++) {
        code = http_get_once(url, body);
        if (code == 200 || code == 404) {
            return code;
        }
        serial_log("[lyrics] GET %s -> HTTP %d (try %d/%d)", url.c_str(), code, attempt, k_tries);
        const bool transient = code < 0 || code == 429 || code >= 500;
        if (!transient || attempt == k_tries || superseded()) {
            break;
        }
        sleep_ms(2000 * attempt);
    }
    return code;
}

/** One LRCLIB record, as far as it matters here. */
struct Record {
    std::string synced, plain;
    bool instrumental = false;
    float dur = 0;
    bool usable() const { return !synced.empty() || !plain.empty() || instrumental; }
};

/** Keep only these fields (a search result is mostly other records' lyrics otherwise). */
template <typename T>
static void fill_filter(T f) {
    f["duration"] = true;
    f["instrumental"] = true;
    f["syncedLyrics"] = true;
    f["plainLyrics"] = true;
}

static Record record_from(JsonObjectConst o) {
    Record rec;
    rec.synced = o["syncedLyrics"] | "";
    rec.plain = o["plainLyrics"] | "";
    rec.instrumental = o["instrumental"] | false;
    rec.dur = o["duration"] | 0.0f;
    return rec;
}

enum class Fetch { Found, NotFound, Error };

/** /api/get: exact match (title, artist, duration). */
static Fetch fetch_exact(const Request &q, const std::string &title, const std::string &artist, Record *out) {
    std::string url = std::string(k_api) + "/get?track_name=" + url_encode(title) + "&artist_name=" + url_encode(artist);
    if (q.dur_s > 0) {
        url += "&duration=" + std::to_string((int)lroundf(q.dur_s));
    }
    std::string body;
    const int code = http_get(url, &body);
    if (code == 404) {
        return Fetch::NotFound;
    }
    if (code != 200) {
        return Fetch::Error;
    }
    JsonDocument filter;
    fill_filter(filter.to<JsonObject>());
    JsonDocument doc(&s_json_alloc);
    const DeserializationError jerr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (jerr) {
        serial_log("[lyrics] get: JSON error %s (%u bytes)", jerr.c_str(), (unsigned)body.size());
        return Fetch::Error;
    }
    *out = record_from(doc.as<JsonObjectConst>());
    return out->usable() ? Fetch::Found : Fetch::NotFound;
}

/** /api/search with the given query string; picks the best record for the track's duration. */
static Fetch fetch_search(const Request &q, const std::string &query, Record *out) {
    std::string body;
    const int code = http_get(std::string(k_api) + "/search?" + query, &body);
    if (code != 200) {
        return Fetch::Error;
    }
    JsonDocument filter;
    fill_filter(filter[0]);
    JsonDocument doc(&s_json_alloc);
    body.shrink_to_fit();
    const DeserializationError jerr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (jerr) {
        serial_log("[lyrics] search: JSON error %s (%u bytes)", jerr.c_str(), (unsigned)body.size());
        return Fetch::Error;
    }
    body = std::string(); /* free before copying lyrics out */

    int best = -1;
    int best_rank = 99;
    float best_diff = 1e9f;
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    for (size_t i = 0; i < arr.size(); i++) {
        JsonObjectConst o = arr[i].as<JsonObjectConst>();
        const float d = o["duration"] | 0.0f;
        const float diff = q.dur_s > 0 && d > 0 ? fabsf(d - q.dur_s) : 0;
        if (diff > k_max_dur_diff_s) {
            continue;
        }
        const char *synced = o["syncedLyrics"] | "";
        const char *plain = o["plainLyrics"] | "";
        const int rank = synced[0] ? 0 : plain[0] ? 1 : (o["instrumental"] | false) ? 2 : 99;
        if (rank < best_rank || (rank == best_rank && diff < best_diff)) {
            best = (int)i;
            best_rank = rank;
            best_diff = diff;
        }
    }
    if (best < 0 || best_rank == 99) {
        return Fetch::NotFound;
    }
    *out = record_from(arr[best].as<JsonObjectConst>());
    return Fetch::Found;
}

/** Artist and title to search for: the tag's, else "Artist - Title" from the file name. */
static void split_name(const Request &q, std::string *artist, std::string *title) {
    *artist = trim(q.artist);
    *title = trim(q.title);
    if (artist->empty()) {
        const size_t dash = title->find(" - ");
        if (dash != std::string::npos) {
            *artist = trim(title->substr(0, dash));
            *title = trim(title->substr(dash + 3));
        }
    }
    std::replace(title->begin(), title->end(), '_', ' ');
}

/** Try LRCLIB step by step. Found: *out is the record; Error: no answer (network / server). */
static Fetch fetch_lrclib(const Request &q, Record *out) {
    std::string artist, title;
    split_name(q, &artist, &title);
    if (title.empty()) {
        return Fetch::NotFound;
    }
    serial_log("[lyrics] looking up artist \"%s\", title \"%s\", %.0f s", artist.c_str(), title.c_str(), q.dur_s);
    bool any_error = false;
    Fetch f = Fetch::NotFound;
    if (!artist.empty()) {
        if ((f = fetch_exact(q, title, artist, out)) == Fetch::Found) {
            return f;
        }
        any_error |= f == Fetch::Error;
        if (superseded() || (f = fetch_search(q, "track_name=" + url_encode(title) + "&artist_name=" + url_encode(artist), out)) == Fetch::Found) {
            return f;
        }
        any_error |= f == Fetch::Error;
    }
    if (superseded() || (f = fetch_search(q, "track_name=" + url_encode(title), out)) == Fetch::Found) {
        return f;
    }
    any_error |= f == Fetch::Error;
    const std::string words = artist.empty() ? title : artist + " " + title;
    if (superseded() || (f = fetch_search(q, "q=" + url_encode(words), out)) == Fetch::Found) {
        return f;
    }
    any_error |= f == Fetch::Error;
    return any_error ? Fetch::Error : Fetch::NotFound;
}

/** The whole lookup for one request. nullptr if a newer request took over. */
static LyricsResult *find(const Request &q) {
    LyricsResult *r = new LyricsResult();
    r->id = q.id;
    const std::string lrc = lrc_path(q.path);
    const std::string miss = miss_path(q.path);
    if (q.retry) {
        (void)unlink(sd_path(miss.c_str()).c_str());
    }

    std::string text;
    if (sd_read_text(lrc.c_str(), &text)) {
        parse_lrc(text, r);
        serial_log("[lyrics] %s: %u line(s) from %s", q.path.c_str(), (unsigned)r->lines.size(), lrc.c_str());
        if (r->kind == LyricsKind::None) {
            r->note = "The .lrc file has no lyrics";
        }
        return r;
    }
    if (card_exists(miss)) {
        r->note = "No lyrics found";
        r->can_retry = true;
        return r;
    }

    /* Wait for Wi-Fi (e.g. right after boot), telling the UI once. */
    if (!wifi_net_connected()) {
        LyricsResult *wait = new LyricsResult();
        wait->id = q.id;
        wait->note = "Waiting for Wi-Fi to look up lyrics...";
        wait->final = false;
        publish(wait);
        while (!wifi_net_connected()) {
            if (superseded()) {
                delete r;
                return nullptr;
            }
            sleep_ms(1000);
        }
    }

    const uint32_t t0 = now_ms();
    Record rec;
    const Fetch f = fetch_lrclib(q, &rec);
    if (superseded()) {
        delete r;
        return nullptr;
    }
    if (f == Fetch::Error) {
        serial_log("[lyrics] %s: lookup failed (%lu ms)", q.path.c_str(), (unsigned long)(now_ms() - t0));
        r->note = "Couldn't reach the lyrics service";
        r->can_retry = true;
        return r;
    }
    if (f == Fetch::NotFound) {
        serial_log("[lyrics] %s: not found (%lu ms)", q.path.c_str(), (unsigned long)(now_ms() - t0));
        mark_missing(q.path);
        r->note = "No lyrics found";
        r->can_retry = true;
        return r;
    }

    /* Save as .lrc next to the song (synced, plain text, or a one-line instrumental note). */
    std::string out = "[re:lrclib.net]\n";
    if (!rec.synced.empty()) {
        out += rec.synced;
    } else if (!rec.plain.empty()) {
        out += rec.plain;
    } else {
        out += "[00:00.00](Instrumental)";
    }
    out += "\n";
    if (!sd_write_text(lrc.c_str(), out)) {
        serial_log("[lyrics] cannot write %s", lrc.c_str());
    }
    parse_lrc(out, r);
    serial_log("[lyrics] %s: %s, %u line(s), %.0f s (track %.0f s), %lu ms", q.path.c_str(),
               r->kind == LyricsKind::Synced ? "synced" : "plain", (unsigned)r->lines.size(), rec.dur, q.dur_s,
               (unsigned long)(now_ms() - t0));
    return r;
}

static void lyrics_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (!s_req_new) {
                xSemaphoreGive(s_mtx);
                break;
            }
            const Request q = s_req;
            s_req_new = false;
            xSemaphoreGive(s_mtx);
            LyricsResult *r = find(q);
            if (r) {
                publish(r);
            }
        }
    }
}

/* ---------- API ---------- */

void lyrics_request(const std::string &path, const std::string &title, const std::string &artist, float dur_s,
                    uint32_t id, bool retry) {
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        /* PSRAM stack: never touch flash (NVS etc.) here. 12 KB: TLS handshake. */
        xTaskCreatePinnedToCoreWithCaps(lyrics_task, "lyrics", 12288, nullptr, 1, &s_task, 1, MALLOC_CAP_SPIRAM);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_req.path = path;
    s_req.title = title;
    s_req.artist = artist;
    s_req.dur_s = dur_s;
    s_req.id = id;
    s_req.retry = retry;
    s_req_new = true;
    xSemaphoreGive(s_mtx);
    xTaskNotifyGive(s_task);
}

LyricsResult *lyrics_take(void) {
    if (!s_mtx) {
        return nullptr;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    LyricsResult *r = s_done;
    s_done = nullptr;
    xSemaphoreGive(s_mtx);
    return r;
}
