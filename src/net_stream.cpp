#include "net_stream.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

#include "platform_time.h"
#include "serial_log.h"

/*
 * Ring buffer: PSRAM, guarded by s_ring_mtx (not a FreeRTOS stream buffer: those can't be reset
 * while the player waits on them, and a station change must drop what's buffered). s_data / s_space
 * wake the reader / writer. Sessions are numbered (s_gen): open and close bump it, and the stream
 * task drops a session whose number is stale, so nothing from an old station ends up in the buffer.
 *
 * ICY metadata: with "Icy-MetaData: 1" the server inserts, every icy-metaint bytes of audio, one
 * length byte (x16) and that much text ("StreamTitle='Artist - Song';...").
 */

static const char *k_user_agent = "JC3248W535EN-radio/1.0 (https://github.com/Yuanzjls/JC3248W535EN)";
static const size_t k_ring_size = 192 * 1024; /* ~12 s at 128 kbps */
static const size_t k_prebuffer = 32 * 1024;  /* start (and resume after an underrun) at ~2 s */
static const size_t k_chunk = 4096;
static const int k_max_redirects = 5;
static const int k_max_retries = 5;

static uint8_t *s_ring = nullptr;
static size_t s_head = 0, s_tail = 0, s_fill = 0;
static bool s_buffering = true;
static SemaphoreHandle_t s_ring_mtx = nullptr;
static SemaphoreHandle_t s_data = nullptr;
static SemaphoreHandle_t s_space = nullptr;

static SemaphoreHandle_t s_mtx = nullptr; /* s_url, s_info, s_state */
static TaskHandle_t s_task = nullptr;
static volatile uint32_t s_gen = 0;
static std::string s_url;
static StreamState s_state = StreamState::Idle;
static std::string s_now_playing, s_error;

/* ---------- ring buffer ---------- */

static void ring_reset(void) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    s_head = s_tail = s_fill = 0;
    s_buffering = true;
    xSemaphoreGive(s_ring_mtx);
}

/** Append; waits for space. False once session `gen` is stale. */
static bool ring_write(const uint8_t *src, size_t n, uint32_t gen) {
    while (n > 0) {
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        if (gen != s_gen) { /* checked under the lock: open/close bump s_gen before resetting the ring */
            xSemaphoreGive(s_ring_mtx);
            return false;
        }
        size_t k = k_ring_size - s_fill;
        k = k < n ? k : n;
        for (size_t done = 0; done < k;) { /* at most two pieces (wrap-around) */
            const size_t run = std::min(k - done, k_ring_size - s_head);
            memcpy(s_ring + s_head, src + done, run);
            s_head = (s_head + run) % k_ring_size;
            done += run;
        }
        s_fill += k;
        xSemaphoreGive(s_ring_mtx);
        src += k;
        n -= k;
        if (k > 0) {
            xSemaphoreGive(s_data);
        } else {
            xSemaphoreTake(s_space, pdMS_TO_TICKS(100));
        }
    }
    return true;
}

size_t stream_read(uint8_t *dst, size_t len, uint32_t timeout_ms) {
    if (!s_ring_mtx) {
        return 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        if (s_buffering && s_fill >= k_prebuffer) {
            s_buffering = false;
        }
        size_t k = 0;
        if (!s_buffering) {
            k = s_fill < len ? s_fill : len;
            for (size_t done = 0; done < k;) {
                const size_t run = std::min(k - done, k_ring_size - s_tail);
                memcpy(dst + done, s_ring + s_tail, run);
                s_tail = (s_tail + run) % k_ring_size;
                done += run;
            }
            s_fill -= k;
            if (s_fill == 0) {
                s_buffering = true; /* ran dry: rebuffer before playing on */
            }
        }
        xSemaphoreGive(s_ring_mtx);
        if (k > 0) {
            xSemaphoreGive(s_space);
            return k;
        }
        if (pass == 0) {
            xSemaphoreTake(s_data, pdMS_TO_TICKS(timeout_ms));
        }
    }
    return 0;
}

/* ---------- state ---------- */

static void set_state(uint32_t gen, StreamState st, const char *error = nullptr) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (gen == s_gen) {
        s_state = st;
        s_error = error ? error : "";
    }
    xSemaphoreGive(s_mtx);
}

bool stream_failed(void) {
    if (!s_mtx) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool f = s_state == StreamState::Failed;
    xSemaphoreGive(s_mtx);
    return f;
}

void stream_get_info(StreamInfo *out) {
    *out = StreamInfo();
    if (!s_mtx) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out->state = s_state;
    out->now_playing = s_now_playing;
    out->error = s_error;
    xSemaphoreGive(s_mtx);
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    out->buffer_pct = (int)(s_fill * 100 / k_ring_size);
    const bool buffering = s_buffering;
    xSemaphoreGive(s_ring_mtx);
    if (out->state == StreamState::Playing && buffering) {
        out->state = StreamState::Buffering;
    }
}

/* ---------- ICY metadata ---------- */

/** Latin-1 -> UTF-8 when the text isn't valid UTF-8 already (stations use both). */
static std::string to_utf8(const std::string &s) {
    bool valid = true;
    for (size_t i = 0; i < s.size() && valid;) {
        const unsigned char c = s[i];
        const int follow = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        valid = follow >= 0;
        for (int k = 1; valid && k <= follow; k++) {
            valid = i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80;
        }
        i += follow + 1;
    }
    if (valid) {
        return s;
    }
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += (char)c;
        } else {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static void parse_icy(const std::string &meta, uint32_t gen) {
    const size_t a = meta.find("StreamTitle='");
    if (a == std::string::npos) {
        return;
    }
    const size_t from = a + 13;
    size_t to = meta.find("';", from);
    if (to == std::string::npos) {
        to = meta.rfind('\'');
    }
    if (to == std::string::npos || to < from) {
        return;
    }
    const std::string title = to_utf8(meta.substr(from, to - from));
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool changed = gen == s_gen && title != s_now_playing;
    if (changed) {
        s_now_playing = title;
    }
    xSemaphoreGive(s_mtx);
    if (changed) {
        serial_log("[stream] now playing: %s", title.c_str());
    }
}

/* ---------- HTTP ---------- */

struct Headers {
    int metaint = 0;
    std::string content_type;
};

static esp_err_t on_http_event(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data) {
        Headers *h = (Headers *)e->user_data;
        if (strcasecmp(e->header_key, "icy-metaint") == 0) {
            h->metaint = atoi(e->header_value);
        } else if (strcasecmp(e->header_key, "content-type") == 0) {
            h->content_type = e->header_value;
        }
    }
    return ESP_OK;
}

enum class Session { Stopped, Dropped, Fatal };

/** Connect and pump audio into the ring until the stream ends, fails or is replaced. */
static Session run_once(const std::string &url, uint32_t gen, bool *got_data) {
    Headers hdr;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.user_agent = k_user_agent;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = on_http_event;
    cfg.user_data = &hdr;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return Session::Dropped;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    Session result = Session::Dropped;
    int code = -1;
    for (int redirects = 0;; redirects++) {
        hdr = Headers();
        const esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            serial_log("[stream] connect failed: %s", esp_err_to_name(err));
            break;
        }
        (void)esp_http_client_fetch_headers(client);
        code = esp_http_client_get_status_code(client);
        const bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect || redirects >= k_max_redirects) {
            break;
        }
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
    }

    uint8_t *buf = code == 200 ? (uint8_t *)heap_caps_malloc(k_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    const std::string &ct = hdr.content_type;
    if (code >= 400 && code < 500) {
        serial_log("[stream] HTTP %d", code);
        set_state(gen, StreamState::Failed, "Station offline");
        result = Session::Fatal;
    } else if (code == 200 && (strcasestr(ct.c_str(), "aac") || strcasestr(ct.c_str(), "ogg") ||
                               strcasestr(ct.c_str(), "mpegurl") || strcasestr(ct.c_str(), "flac"))) {
        serial_log("[stream] unsupported content type %s", ct.c_str());
        set_state(gen, StreamState::Failed, "Not an MP3 stream");
        result = Session::Fatal;
    } else if (buf) {
        serial_log("[stream] connected (%s, metaint %d; free internal %u, largest block %u)", ct.c_str(), hdr.metaint,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        set_state(gen, StreamState::Playing);
        int until_meta = hdr.metaint; /* audio bytes before the next metadata block */
        int meta_left = -1;           /* metadata bytes still to read; -1: next byte is its length */
        std::string meta;
        result = Session::Dropped;
        for (;;) {
            if (gen != s_gen) {
                result = Session::Stopped;
                break;
            }
            const int n = esp_http_client_read(client, (char *)buf, k_chunk);
            if (n <= 0) {
                serial_log("[stream] connection ended (%d)", n);
                break;
            }
            *got_data = true;
            int i = 0;
            bool ok = true;
            while (i < n && ok) {
                if (hdr.metaint <= 0) {
                    ok = ring_write(buf, n, gen);
                    i = n;
                } else if (until_meta > 0) {
                    const int k = std::min(until_meta, n - i);
                    ok = ring_write(buf + i, k, gen);
                    until_meta -= k;
                    i += k;
                } else if (meta_left < 0) {
                    meta_left = buf[i++] * 16;
                    meta.clear();
                    if (meta_left == 0) {
                        until_meta = hdr.metaint;
                        meta_left = -1;
                    }
                } else {
                    const int k = std::min(meta_left, n - i);
                    meta.append((const char *)buf + i, k);
                    meta_left -= k;
                    i += k;
                    if (meta_left == 0) {
                        parse_icy(meta, gen);
                        until_meta = hdr.metaint;
                        meta_left = -1;
                    }
                }
            }
            if (!ok) {
                result = Session::Stopped;
                break;
            }
        }
    } else if (code >= 0) {
        serial_log("[stream] HTTP %d", code);
    }
    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

/** One station until it is replaced or closed; reconnects after drops, gives up after k_max_retries. */
static void run_session(const std::string &url, uint32_t gen) {
    ring_reset();
    int failures = 0;
    while (gen == s_gen) {
        set_state(gen, failures ? StreamState::Retrying : StreamState::Connecting, failures ? "Reconnecting..." : nullptr);
        bool got_data = false;
        const Session r = run_once(url, gen, &got_data);
        if (r != Session::Dropped || gen != s_gen) {
            return; /* stopped, or Failed already set */
        }
        failures = got_data ? 1 : failures + 1; /* was playing: retry promptly */
        if (failures > k_max_retries) {
            set_state(gen, StreamState::Failed, "Station not reachable");
            return;
        }
        for (int t = 0; t < failures * 10 && gen == s_gen; t++) { /* 1 s, 2 s, ... */
            sleep_ms(100);
        }
    }
}

static void stream_task(void *arg) {
    (void)arg;
    uint32_t done = 0; /* last session handled */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            const uint32_t gen = s_gen;
            const std::string url = s_url;
            xSemaphoreGive(s_mtx);
            if (gen == done) {
                break;
            }
            done = gen;
            if (!url.empty()) {
                serial_log("[stream] opening %s", url.c_str());
                run_session(url, gen);
            }
        }
    }
}

/* ---------- API ---------- */

static bool ensure_started(void) {
    if (s_task) {
        return true;
    }
    s_ring = (uint8_t *)heap_caps_malloc(k_ring_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        serial_log("[stream] out of memory");
        return false;
    }
    s_ring_mtx = xSemaphoreCreateMutex();
    s_data = xSemaphoreCreateBinary();
    s_space = xSemaphoreCreateBinary();
    s_mtx = xSemaphoreCreateMutex();
    /* Core 1 below the audio task (5); PSRAM stack: never touch flash (NVS etc.) here. 12 KB: TLS. */
    xTaskCreatePinnedToCoreWithCaps(stream_task, "stream", 12288, nullptr, 4, &s_task, 1, MALLOC_CAP_SPIRAM);
    return s_task != nullptr;
}

void stream_open(const std::string &url) {
    if (!ensure_started()) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_url = url;
    s_gen = s_gen + 1;
    s_state = StreamState::Connecting;
    s_now_playing.clear();
    s_error.clear();
    xSemaphoreGive(s_mtx);
    ring_reset();
    xSemaphoreGive(s_space); /* wake a writer waiting for space: its session is stale now */
    xTaskNotifyGive(s_task);
}

void stream_close(void) {
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_url.clear();
    s_gen = s_gen + 1;
    s_state = StreamState::Idle;
    s_now_playing.clear();
    s_error.clear();
    xSemaphoreGive(s_mtx);
    ring_reset();
    xSemaphoreGive(s_space);
    xTaskNotifyGive(s_task);
}
