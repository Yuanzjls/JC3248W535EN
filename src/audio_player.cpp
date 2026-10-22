#include "audio_player.h"

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <utility>

#include "app_settings.h"
#include "minimp3/minimp3.h"
#include "sd_card.h"
#include "serial_log.h"

/*
 * NS4168 class-D amplifier on I2S (schematic: DIN IO41, LRCLK IO2, BCLK IO42; CTRL is pulled up to
 * the battery rail, so the amp is always on). It plays a single channel, so stereo tracks are
 * mixed down to mono and sent on both slots. Volume is a software gain.
 *
 * One task owns the decoder, the open file and I2S; the UI talks to it through a command queue
 * and reads a status snapshot under s_lock. Core 1: it never sends display frames (those must stay
 * on core 0), and core 0 is busy with LVGL and video decoding.
 *
 * Decoder: minimp3 (src/minimp3, built with -O2). mp3dec_decode_frame() keeps ~16 KB of scratch on
 * the stack, hence the large task stack.
 *
 * Internal RAM is scarce (BLE alone takes ~44 KB, and the SD driver needs some for DMA), so the
 * stack, decoder state and buffers live in PSRAM; only the I2S DMA buffers (~7 KB) are internal.
 * The 16 KB input buffer, not I2S, absorbs slow SD reads.
 */

static const gpio_num_t k_pin_bclk = GPIO_NUM_42;
static const gpio_num_t k_pin_ws = GPIO_NUM_2;
static const gpio_num_t k_pin_dout = GPIO_NUM_41;

static const size_t k_in_buf = 16 * 1024;       /* minimp3 syncs on several frames in a row: give it plenty */
static const int k_refill_at = 8 * 1024;
static const uint32_t k_task_stack = 24 * 1024;
static const uint32_t k_max_text = 1024;        /* longest ID3 title / artist frame read */
static const size_t k_pic_head = 256;           /* APIC bytes read to find where the picture starts */
static const float k_restart_prev_s = 3.0f;     /* Prev restarts the track after this */
static const int k_max_tries = 64;              /* decode calls without a frame before giving up */

enum class Cmd : uint8_t { Play, Toggle, Next, Prev, Stop, Seek, Shuffle };
struct Command {
    Cmd cmd;
    uint32_t arg; /* Play: index; Seek: permille; Shuffle: on */
};

static QueueHandle_t s_cmd_q = nullptr;
static SemaphoreHandle_t s_lock = nullptr; /* s_playlist, s_status */
static std::vector<std::string> s_playlist;
static AudioStatus s_status;
static volatile int s_gain_q15 = 0; /* volume as a Q15 gain */
static volatile bool s_shuffle = false;

/* ---------- task-only state ---------- */

static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_i2s_rate = 44100;
static mp3dec_t *s_dec = nullptr; /* PSRAM */

struct Track {
    int fd = -1;
    uint32_t start = 0, end = 0; /* audio bytes [start, end) - after ID3v2, before ID3v1 */
    uint32_t pos = 0;            /* next file offset to read */
    uint8_t *buf = nullptr;
    uint8_t *ptr = nullptr;      /* unread data in buf */
    int left = 0;
    uint64_t samples = 0; /* per channel, since base_s */
    float base_s = 0;
    float dur_s = 0;
    bool has_toc = false;
    uint8_t toc[100];
};
static Track s_t;
static int16_t *s_pcm = nullptr;

/* ---------- helpers ---------- */

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

bool audio_is_mp3(const char *name) {
    const size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t syncsafe32(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) |
           (p[3] & 0x7F);
}

static bool read_at(int fd, uint32_t off, void *dst, size_t len) {
    return lseek(fd, off, SEEK_SET) == (off_t)off && read(fd, dst, len) == (ssize_t)len;
}

static void append_utf8(std::string &out, uint32_t c) {
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

/** ID3v2 text frame body (encoding byte + text) as UTF-8. */
static std::string id3_text(const uint8_t *p, size_t n) {
    std::string out;
    if (n < 2) {
        return out;
    }
    const uint8_t enc = p[0];
    p++;
    n--;
    if (enc == 0 || enc == 3) { /* ISO-8859-1 / UTF-8 */
        for (size_t i = 0; i < n && p[i]; i++) {
            if (enc == 0) {
                append_utf8(out, p[i]);
            } else {
                out += (char)p[i];
            }
        }
    } else { /* UTF-16: 1 = with BOM, 2 = big-endian */
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
            be = p[0] == 0xFE;
            i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t c = be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (c == 0) {
                break;
            }
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                const uint32_t lo = be ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            append_utf8(out, c);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
        out.pop_back();
    }
    return out;
}

struct TagInfo {
    std::string title, artist;
    uint32_t cover_off = 0, cover_len = 0;
    bool cover_front = false; /* picture type 3 "front cover" */
};

/**
 * Where the picture data starts in an APIC / PIC frame body (encoding, MIME type or 3-letter
 * format, picture type, description, data). 0 if the header doesn't fit in `n` bytes.
 */
static size_t picture_data_start(const uint8_t *b, size_t n, bool v22, uint8_t *type) {
    const uint8_t enc = b[0];
    size_t i = 1;
    if (v22) {
        i += 3;
    } else {
        while (i < n && b[i]) {
            i++;
        }
        i++;
    }
    if (i >= n) {
        return 0;
    }
    *type = b[i++];
    if (enc == 1 || enc == 2) { /* UTF-16 description: ends with a 16-bit zero */
        while (i + 1 < n && (b[i] || b[i + 1])) {
            i += 2;
        }
        i += 2;
    } else {
        while (i < n && b[i]) {
            i++;
        }
        i++;
    }
    return i < n ? i : 0;
}

/**
 * Walk the ID3v2 tag at the start of the file frame by frame (only small reads, so a large cover
 * image costs nothing here). Returns where the audio starts; fills title, artist and the cover's
 * location.
 */
static uint32_t parse_id3v2(int fd, TagInfo *tag) {
    uint8_t h[10];
    if (!read_at(fd, 0, h, sizeof(h)) || memcmp(h, "ID3", 3) != 0) {
        return 0;
    }
    const uint8_t ver = h[3];
    const uint8_t flags = h[5];
    const uint32_t tag_end = 10 + syncsafe32(h + 6);
    const uint32_t audio_start = tag_end + ((flags & 0x10) ? 10 : 0);
    if (ver < 2 || ver > 4) {
        return audio_start;
    }
    /* Whole-tag unsynchronisation (v2.2 / v2.3) alters binary data: text is fine, pictures are not. */
    const bool tag_unsync = ver < 4 && (flags & 0x80);

    uint32_t pos = 10;
    if ((flags & 0x40) && ver >= 3) { /* extended header */
        uint8_t x[4];
        if (!read_at(fd, pos, x, sizeof(x))) {
            return audio_start;
        }
        pos += ver == 4 ? syncsafe32(x) : 4 + be32(x);
    }
    const uint32_t hdr = ver == 2 ? 6 : 10;
    uint8_t *body = (uint8_t *)malloc(k_max_text > k_pic_head ? k_max_text : k_pic_head);
    if (!body) {
        return audio_start;
    }
    for (int frames = 0; frames < 200 && pos + hdr <= tag_end; frames++) {
        uint8_t f[10];
        if (!read_at(fd, pos, f, hdr) || f[0] == 0) {
            break; /* padding */
        }
        const uint32_t fsize = ver == 2 ? ((uint32_t)f[3] << 16 | f[4] << 8 | f[5]) : ver == 4 ? syncsafe32(f + 4) : be32(f + 4);
        if (fsize == 0 || pos + hdr + fsize > tag_end) {
            break;
        }
        /* Frame flags: skip compressed / encrypted / unsynchronised frames, step over prefixes. */
        bool plain = true;
        uint32_t skip = 0;
        if (ver == 4) {
            plain = !(f[9] & 0x0E);
            skip = (f[9] & 0x01) ? 4 : 0; /* data length indicator */
        } else if (ver == 3) {
            plain = !(f[9] & 0xC0);
            skip = (f[9] & 0x20) ? 1 : 0; /* group id */
        }
        const uint32_t data = pos + hdr + skip;
        const uint32_t len = fsize > skip ? fsize - skip : 0;

        const bool is_title = ver == 2 ? memcmp(f, "TT2", 3) == 0 : memcmp(f, "TIT2", 4) == 0;
        const bool is_artist = ver == 2 ? memcmp(f, "TP1", 3) == 0 : memcmp(f, "TPE1", 4) == 0;
        const bool is_pic = ver == 2 ? memcmp(f, "PIC", 3) == 0 : memcmp(f, "APIC", 4) == 0;
        if ((is_title || is_artist) && plain && len <= k_max_text && read_at(fd, data, body, len)) {
            std::string text = id3_text(body, len);
            if (!text.empty()) {
                *(is_title ? &tag->title : &tag->artist) = text;
            }
        } else if (is_pic && plain && !tag_unsync && !tag->cover_front) {
            const size_t n = len < k_pic_head ? len : k_pic_head;
            uint8_t type = 0;
            const size_t at = read_at(fd, data, body, n) ? picture_data_start(body, n, ver == 2, &type) : 0;
            if (at > 0 && (tag->cover_len == 0 || type == 3)) { /* first picture, or the front cover */
                tag->cover_off = data + at;
                tag->cover_len = len - at;
                tag->cover_front = type == 3;
            }
        }
        pos += hdr + fsize;
    }
    free(body);
    return audio_start;
}

/** "Some Song.mp3" from "/music/Some Song.mp3", without the extension. */
static std::string display_name(const std::string &path) {
    const size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (audio_is_mp3(name.c_str())) {
        name.resize(name.size() - 4);
    }
    return name;
}

/* ---------- I2S ---------- */

static bool i2s_begin(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 300; /* ~40 ms at 44.1 kHz in 7 KB of internal RAM: covers decode jitter */
    chan_cfg.auto_clear = true;   /* silence (not the last buffer looping) while paused or starved */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) {
        serial_log("[audio] i2s_new_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_i2s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = k_pin_bclk,
                .ws = k_pin_ws,
                .dout = k_pin_dout,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {},
            },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        serial_log("[audio] I2S init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    return true;
}

static void i2s_set_rate(uint32_t rate) {
    if (rate == s_i2s_rate || rate == 0) {
        return;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(s_tx);
    if (i2s_channel_reconfig_std_clock(s_tx, &clk) == ESP_OK) {
        s_i2s_rate = rate;
    }
    i2s_channel_enable(s_tx);
}

/* ---------- track ---------- */

static void track_close(void) {
    if (s_t.fd >= 0) {
        close(s_t.fd);
    }
    uint8_t *buf = s_t.buf;
    s_t = Track();
    s_t.buf = buf;
}

/** Move unread data to the front and read more. False if nothing more could be read. */
static bool track_refill(void) {
    if (s_t.left > 0 && s_t.ptr != s_t.buf) {
        memmove(s_t.buf, s_t.ptr, s_t.left);
    }
    s_t.ptr = s_t.buf;
    size_t want = k_in_buf - s_t.left;
    if (want > s_t.end - s_t.pos) {
        want = s_t.end - s_t.pos;
    }
    if (want == 0) {
        return false;
    }
    const size_t got = sd_read(s_t.fd, s_t.buf + s_t.left, want);
    s_t.pos += got;
    s_t.left += got;
    return got > 0;
}

/** Read from file offset `at` with an empty buffer and a fresh decoder. */
static void track_reposition(uint32_t at) {
    mp3dec_init(s_dec);
    s_t.pos = at;
    s_t.left = 0;
    s_t.ptr = s_t.buf;
    if (lseek(s_t.fd, at, SEEK_SET) == (off_t)at) {
        (void)track_refill();
    }
}

/**
 * Length from the first frame: Xing/Info header (VBR; also gives the seek table), VBRI, else
 * constant bitrate. Fills *rate and *kbps (0 if no frame was found).
 */
static void track_probe(uint32_t *rate, uint32_t *kbps) {
    mp3dec_frame_info_t info = {};
    const int spf = mp3dec_decode_frame(s_dec, s_t.ptr, s_t.left, nullptr, &info); /* header only */
    mp3dec_init(s_dec);
    *rate = spf > 0 ? info.hz : 0;
    *kbps = spf > 0 ? info.bitrate_kbps : 0;
    if (spf <= 0 || info.hz <= 0) {
        return;
    }
    const uint8_t *f = s_t.ptr + info.frame_offset;
    const int avail = s_t.left - info.frame_offset;
    const bool mpeg1 = (f[1] & 0x18) == 0x18;
    const int side = mpeg1 ? (info.channels == 1 ? 17 : 32) : (info.channels == 1 ? 9 : 17);
    const uint8_t *x = f + 4 + side;
    uint32_t frames = 0;
    if (info.layer == 3 && avail >= 4 + side + 120 && (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0)) {
        const uint32_t flags = be32(x + 4);
        const uint8_t *q = x + 8;
        if (flags & 1) {
            frames = be32(q);
            q += 4;
        }
        if (flags & 2) {
            q += 4;
        }
        if (flags & 4) {
            memcpy(s_t.toc, q, sizeof(s_t.toc));
            s_t.has_toc = true;
        }
    } else if (avail >= 36 + 18 && memcmp(f + 36, "VBRI", 4) == 0) {
        frames = be32(f + 36 + 14);
    }
    if (frames > 0) {
        s_t.dur_s = (float)frames * spf / info.hz;
    } else if (info.bitrate_kbps > 0) {
        s_t.dur_s = (s_t.end - s_t.start) * 8.0f / (info.bitrate_kbps * 1000.0f);
    }
}

static bool track_open(size_t index) {
    track_close();
    std::string path;
    lock();
    if (index < s_playlist.size()) {
        path = s_playlist[index];
    }
    s_status.index = index;
    s_status.count = s_playlist.size();
    s_status.path = path;
    s_status.title = display_name(path);
    s_status.artist.clear();
    s_status.pos_s = s_status.dur_s = 0;
    s_status.sample_rate = s_status.bitrate_kbps = 0;
    s_status.cover_off = s_status.cover_len = 0;
    s_status.error = false;
    unlock();
    if (path.empty()) {
        return false;
    }

    s_t.fd = sd_open_read(path.c_str());
    const uint32_t size = s_t.fd >= 0 ? (uint32_t)sd_file_size(s_t.fd) : 0;
    if (size == 0) {
        serial_log("[audio] cannot open %s", path.c_str());
        track_close();
        return false;
    }
    TagInfo tag;
    s_t.start = parse_id3v2(s_t.fd, &tag);
    s_t.end = size;
    uint8_t v1[3];
    if (size >= 128 && read_at(s_t.fd, size - 128, v1, sizeof(v1)) && memcmp(v1, "TAG", 3) == 0) {
        s_t.end = size - 128; /* ID3v1 */
    }
    if (tag.cover_off + tag.cover_len > size) {
        tag.cover_len = 0;
    }
    if (s_t.start >= s_t.end) {
        track_close();
        return false;
    }
    track_reposition(s_t.start);
    uint32_t rate = 0, kbps = 0;
    track_probe(&rate, &kbps);

    lock();
    if (!tag.title.empty()) {
        s_status.title = tag.title;
    }
    s_status.artist = tag.artist;
    s_status.dur_s = s_t.dur_s;
    s_status.sample_rate = rate;
    s_status.bitrate_kbps = kbps;
    s_status.cover_off = tag.cover_off;
    s_status.cover_len = tag.cover_len;
    s_status.seq++;
    unlock();
    serial_log("[audio] %s: %lu Hz, %lu kbps, %.0f s%s%s", path.c_str(), (unsigned long)rate, (unsigned long)kbps,
               s_t.dur_s, s_t.has_toc ? " (VBR)" : "", tag.cover_len ? ", cover" : "");
    return true;
}

static void track_seek(float fraction) {
    if (s_t.fd < 0 || s_t.dur_s <= 0) {
        return;
    }
    fraction = fraction < 0 ? 0 : fraction > 0.999f ? 0.999f : fraction;
    const uint32_t bytes = s_t.end - s_t.start;
    uint32_t off;
    if (s_t.has_toc) { /* Xing table: byte position (/256) at each percent of the duration */
        const float pct = fraction * 100;
        const int i = (int)pct;
        const float a = s_t.toc[i];
        const float b = i < 99 ? s_t.toc[i + 1] : 256.0f;
        off = (uint32_t)((a + (b - a) * (pct - i)) / 256.0f * bytes);
    } else {
        off = (uint32_t)(fraction * bytes);
    }
    track_reposition(s_t.start + off);
    s_t.base_s = fraction * s_t.dur_s;
    s_t.samples = 0;
}

/** Decode one frame and play it. False at the end of the track (or on unreadable data). */
static bool track_play_frame(void) {
    for (int tries = 0; tries < k_max_tries; tries++) {
        if (s_t.left < k_refill_at) {
            (void)track_refill();
        }
        if (s_t.left <= 0) {
            return false;
        }
        mp3dec_frame_info_t info = {};
        const int n = mp3dec_decode_frame(s_dec, s_t.ptr, s_t.left, s_pcm, &info);
        if (info.frame_bytes == 0) { /* the buffer ends inside a frame */
            if (!track_refill()) {
                return false; /* truncated last frame */
            }
            continue;
        }
        s_t.ptr += info.frame_bytes;
        s_t.left -= info.frame_bytes;
        if (n == 0 || info.channels < 1) {
            continue; /* skipped junk, or the bit reservoir is still filling (start / after a seek) */
        }
        i2s_set_rate(info.hz);

        /* Mono mix with volume, written as identical L/R pairs in place. */
        const int32_t gain = s_gain_q15;
        int16_t *out = s_pcm;
        if (info.channels == 2) {
            for (int i = 0; i < n; i++) {
                const int32_t m = (((int32_t)s_pcm[2 * i] + s_pcm[2 * i + 1]) * gain) >> 16;
                out[2 * i] = out[2 * i + 1] = (int16_t)m;
            }
        } else {
            for (int i = n - 1; i >= 0; i--) { /* backwards: expands in place */
                const int16_t m = (int16_t)(((int32_t)s_pcm[i] * gain) >> 15);
                out[2 * i] = out[2 * i + 1] = m;
            }
        }
        size_t written = 0;
        (void)i2s_channel_write(s_tx, out, (size_t)n * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(1000));
        s_t.samples += n;

        lock();
        s_status.pos_s = s_t.base_s + (float)s_t.samples / info.hz;
        s_status.sample_rate = info.hz;
        if (s_status.bitrate_kbps == 0) {
            s_status.bitrate_kbps = info.bitrate_kbps;
        }
        unlock();
        return true;
    }
    serial_log("[audio] no MP3 data found, skipping track");
    return false;
}

/* ---------- task ---------- */

static void set_state(AudioState st) {
    lock();
    s_status.state = st;
    unlock();
}

static size_t playlist_size(void) {
    lock();
    const size_t n = s_playlist.size();
    unlock();
    return n;
}

/*
 * Play order. Tracks are played by position in an order: the playlist itself, or (shuffle) a
 * random permutation of it, so every track plays once before any repeats. s_pos is the current
 * position; s_order_on says whether s_order is in use (task-side copy of s_shuffle, switched by
 * Cmd::Play / Cmd::Shuffle so the order and the flag always change together).
 */
static std::vector<size_t> s_order;
static size_t s_pos = 0;
static bool s_order_on = false;

/** Playlist index played at order position p. */
static size_t index_at(size_t p) {
    return s_order_on && p < s_order.size() ? s_order[p] : p;
}

/** New random order of n tracks (Fisher-Yates); `first`, if < n, plays first. */
static void shuffle_order(size_t n, size_t first) {
    s_order.resize(n);
    for (size_t i = 0; i < n; i++) {
        s_order[i] = i;
    }
    for (size_t i = n; i > 1; i--) {
        std::swap(s_order[i - 1], s_order[esp_random() % i]);
    }
    for (size_t i = 0; i < n && first < n; i++) {
        if (s_order[i] == first) {
            std::swap(s_order[0], s_order[i]);
            break;
        }
    }
}

/**
 * Open the track at order position `pos`, or the following ones if it fails. At the end of the
 * order: stop, or with `wrap` start over (with a fresh shuffle that doesn't repeat the track just
 * played). Sets the state accordingly.
 */
static void start_at(size_t pos, bool wrap) {
    const size_t n = playlist_size();
    for (size_t tried = 0; tried < n; tried++, pos++) {
        if (pos >= n) {
            if (!wrap) {
                break;
            }
            pos = 0;
            if (s_order_on) {
                lock();
                const size_t last = s_status.index;
                unlock();
                shuffle_order(n, n);
                if (n > 1 && s_order[0] == last) {
                    std::swap(s_order[0], s_order[1]);
                }
            }
        }
        if (track_open(index_at(pos))) {
            s_pos = pos;
            set_state(AudioState::Playing);
            return;
        }
        lock();
        s_status.error = true;
        s_status.seq++;
        unlock();
    }
    track_close();
    lock();
    s_status.state = AudioState::Stopped;
    s_status.pos_s = 0;
    unlock();
}

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        AudioState st;
        lock();
        st = s_status.state;
        unlock();

        Command c;
        if (xQueueReceive(s_cmd_q, &c, st == AudioState::Playing ? 0 : portMAX_DELAY) == pdTRUE) {
            lock();
            const size_t index = s_status.index;
            const float pos = s_status.pos_s;
            unlock();
            switch (c.cmd) {
            case Cmd::Play: /* new playlist: the chosen track first, then in (shuffled) order */
                s_order_on = s_shuffle;
                if (s_order_on) {
                    shuffle_order(playlist_size(), c.arg);
                }
                start_at(s_order_on ? 0 : c.arg, true);
                break;
            case Cmd::Shuffle: /* keep the current track; reorder what comes after it */
                s_order_on = c.arg != 0;
                if (s_order_on) {
                    shuffle_order(playlist_size(), index);
                    s_pos = 0;
                } else {
                    s_pos = index;
                }
                break;
            case Cmd::Toggle:
                if (st == AudioState::Playing) {
                    set_state(AudioState::Paused);
                } else if (st == AudioState::Paused) {
                    set_state(AudioState::Playing);
                } else {
                    start_at(s_pos, true);
                }
                break;
            case Cmd::Next:
                start_at(s_pos + 1, true);
                break;
            case Cmd::Prev:
                if (st != AudioState::Stopped && pos > k_restart_prev_s) {
                    track_seek(0);
                    set_state(AudioState::Playing);
                } else {
                    const size_t n = playlist_size();
                    start_at(s_pos > 0 ? s_pos - 1 : (n ? n - 1 : 0), true);
                }
                break;
            case Cmd::Stop:
                track_close();
                lock();
                s_status.state = AudioState::Stopped;
                s_status.pos_s = 0;
                unlock();
                break;
            case Cmd::Seek:
                track_seek(c.arg / 1000.0f);
                lock();
                s_status.pos_s = s_t.base_s;
                unlock();
                break;
            }
            continue; /* drain further commands before decoding */
        }

        if (!track_play_frame()) {
            start_at(s_pos + 1, false); /* end of the order: stop */
        }
    }
}

static bool s_started = false;

/** Create the queue, I2S channel, buffers and task on first use. */
static bool ensure_started(void) {
    static bool s_failed = false;
    if (s_started || s_failed) {
        return s_started;
    }
    s_failed = true;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_t.buf = (uint8_t *)heap_caps_malloc(k_in_buf, caps); /* sd_read() bounces PSRAM reads through DMA RAM */
    s_pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), caps);
    s_dec = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), caps);
    if (!s_t.buf || !s_pcm || !s_dec || !i2s_begin()) {
        serial_log("[audio] start failed");
        return false;
    }
    mp3dec_init(s_dec);
    /* PSRAM stack: never touch flash (NVS etc.) from this task. */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", k_task_stack, nullptr, 5, nullptr, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        serial_log("[audio] cannot create task");
        return false;
    }
    s_failed = false;
    s_started = true;
    serial_log("[audio] started (I2S BCLK %d, WS %d, DOUT %d; free internal %u)", k_pin_bclk, k_pin_ws, k_pin_dout,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

/** Queue and lock exist from the first call on, even if playback never starts. */
static void ensure_sync(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_cmd_q = xQueueCreate(8, sizeof(Command));
        audio_set_volume(settings_get().volume);
        s_shuffle = settings_get().shuffle;
    }
}

static void send(Cmd cmd, uint32_t arg = 0) {
    ensure_sync();
    if (!ensure_started()) {
        return;
    }
    const Command c = {cmd, arg};
    (void)xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(100));
}

/* ---------- API ---------- */

void audio_play(const std::vector<std::string> &playlist, size_t index) {
    if (index >= playlist.size()) {
        return;
    }
    ensure_sync();
    lock();
    s_playlist = playlist;
    unlock();
    send(Cmd::Play, (uint32_t)index);
}

void audio_toggle_pause(void) {
    send(Cmd::Toggle);
}

void audio_next(void) {
    send(Cmd::Next);
}

void audio_prev(void) {
    send(Cmd::Prev);
}

void audio_stop(void) {
    send(Cmd::Stop);
}

void audio_seek(float fraction) {
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    send(Cmd::Seek, (uint32_t)(fraction * 1000));
}

void audio_set_volume(int percent) {
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_gain_q15 = percent * percent * 32768 / 10000; /* squared: even steps to the ear */
}

void audio_set_shuffle(bool on) {
    ensure_sync();
    s_shuffle = on;
    if (s_started) { /* otherwise the next Play picks it up */
        send(Cmd::Shuffle, on ? 1 : 0);
    }
}

void audio_get_status(AudioStatus *out) {
    ensure_sync();
    lock();
    *out = s_status;
    unlock();
}

std::vector<std::string> audio_playlist(void) {
    ensure_sync();
    lock();
    std::vector<std::string> list = s_playlist;
    unlock();
    return list;
}
