#include "media_ui.h"

#include <lvgl.h> /* before PNGdec: see the #undef below */

#include <JPEGDEC.h>
#include <PNGdec.h>
/* PNGdec's bundled zlib does `#define local static` in a public header. LVGL (lv_meter.h) has a
 * parameter named `local`, so keep the macro from leaking into anything included later. */
#undef local

#include "sd_card.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <new>
#include <strings.h>

#include "esp_bsp.h"
#include "lv_port.h"
#include "serial_log.h"
#include "ui_font.h"
#include "ui_theme.h"

/*
 * Image viewer (JPEG / PNG / BMP) and raw-MJPEG player.
 *
 * Decoding runs in a "media" task on core 1 (core 0 is busy with LVGL redraws). Frames go into one
 * of two RGB565 buffers in PSRAM; the task decodes into the hidden one and then, under the LVGL
 * lock, shows it (stills: point the lv_img at it; video: see the MJPEG section). LVGL only reads a buffer while rendering under that same lock, so
 * the buffer being decoded into is never on screen: no tearing.
 *
 * Pixels are big-endian RGB565 because lv_conf.h has LV_COLOR_16_SWAP=1.
 */

static const int k_scr_w = 480;
static const int k_scr_h = 320;
static const size_t k_max_still_file = 12 * 1024 * 1024; /* bigger images are refused */
static const int k_default_fps = 24;                     /* raw MJPEG has no timing; "_15fps" in the name overrides */
static const uint32_t k_overlay_hide_ms = 3000;

/* ---------- shared state ---------- */

enum class Cmd { ShowImage, PlayVideo, TogglePause, Seek, Stop, Release };
struct MediaCmd {
    Cmd cmd;
    int arg; /* Seek: position in 1/1000 of the file */
    char path[256];
};

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_released = false; /* media task freed its buffers after Back */
static lv_timer_t *s_reaper = nullptr;   /* deletes the viewer screen once released */
static bool ensure_decoders(void);

static uint16_t *s_frame[2] = {nullptr, nullptr};
static lv_img_dsc_t s_dsc[2];
static int s_back = 0; /* buffer the task may write into */

static JPEGDEC *s_jpeg = nullptr;
static PNG *s_png = nullptr;

/* UI (LVGL task only) */
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_img = nullptr;
static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_info = nullptr;
static lv_obj_t *s_msg = nullptr;
static lv_obj_t *s_prev_btn = nullptr;
static lv_obj_t *s_next_btn = nullptr;
static lv_obj_t *s_bottom = nullptr;   /* video controls: play | progress | time */
static lv_obj_t *s_play_btn = nullptr;
static lv_obj_t *s_play_lbl = nullptr;
static lv_obj_t *s_seek_bar = nullptr;
static lv_obj_t *s_time_lbl = nullptr;
static volatile uint32_t s_total_s = 0; /* estimated length, for the drag preview */
static volatile bool s_total_exact = false;
static lv_obj_t *s_return_scr = nullptr;
static lv_timer_t *s_hide_timer = nullptr;

static std::vector<String> s_images;
static size_t s_image_i = 0;
static bool s_video_mode = false;

/* ---------- small helpers ---------- */

static bool ends_with_ci(const String &s, const char *suffix) {
    const size_t n = strlen(suffix);
    return s.length() >= n && strcasecmp(s.c_str() + s.length() - n, suffix) == 0;
}

bool media_is_image(const String &name) {
    return ends_with_ci(name, ".jpg") || ends_with_ci(name, ".jpeg") || ends_with_ci(name, ".png") ||
           ends_with_ci(name, ".bmp");
}

bool media_is_video(const String &name) {
    return ends_with_ci(name, ".mjpeg") || ends_with_ci(name, ".mjpg");
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

/** RGB888 -> big-endian RGB565 (LV_COLOR_16_SWAP). */
static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    return swap16((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
}

static void *psram_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** Largest size that fits the screen, keeping aspect ratio; never upscales. */
static void fit_size(int w, int h, int *fw, int *fh) {
    float f = fminf((float)k_scr_w / w, (float)k_scr_h / h);
    if (f > 1) {
        f = 1;
    }
    *fw = (int)(w * f);
    *fh = (int)(h * f);
    if (*fw < 1) *fw = 1;
    if (*fh < 1) *fh = 1;
}

/** Nearest-neighbour resample src (sw x sh) into dst (dw x dh). */
static void resample(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        const uint16_t *row = src + (size_t)(y * sh / dh) * sw;
        uint16_t *out = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            out[x] = row[x * sw / dw];
        }
    }
}

/* ---------- presenting (task side) ---------- */

static void ui_message(const char *text) {
    bsp_display_lock(0);
    if (s_msg) {
        if (text && text[0]) {
            lv_label_set_text(s_msg, text);
            lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        }
    }
    bsp_display_unlock();
}

static void ui_info(const char *text) {
    bsp_display_lock(0);
    if (s_info) {
        lv_label_set_text(s_info, text);
    }
    bsp_display_unlock();
}

/** Show the back buffer (w x h) and make the other buffer the new back buffer. */
static void present(int w, int h) {
    lv_img_dsc_t &d = s_dsc[s_back];
    memset(&d, 0, sizeof(d));
    d.header.cf = LV_IMG_CF_TRUE_COLOR;
    d.header.w = w;
    d.header.h = h;
    d.data_size = (uint32_t)w * h * 2;
    d.data = (const uint8_t *)s_frame[s_back];

    bsp_display_lock(0);
    if (s_img) {
        lv_img_set_src(s_img, &d);
        lv_obj_center(s_img);
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
    s_back ^= 1;
}

static bool cmd_pending(void) {
    return uxQueueMessagesWaiting(s_queue) > 0;
}

/* ---------- JPEG ---------- */

struct JpegTarget {
    uint16_t *buf; /* top-left of the output area */
    int stride;    /* pixels per buffer row (video: full screen width, letterboxed) */
    int w, h;      /* output area; blocks are clipped to it */
};

static int jpeg_draw_cb(JPEGDRAW *d) {
    const JpegTarget *t = (const JpegTarget *)d->pUser;
    const int w = (d->x + d->iWidthUsed > t->w) ? t->w - d->x : d->iWidthUsed;
    for (int r = 0; r < d->iHeight; r++) {
        const int y = d->y + r;
        if (y >= t->h) {
            break;
        }
        if (w > 0) {
            memcpy(t->buf + (size_t)y * t->stride + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)w * 2);
        }
    }
    return 1;
}

/** Smallest JPEGDEC scale (1, 2, 4, 8) whose output is at most limit_w x limit_h. */
static int jpeg_scale_for(int w, int h, int limit_w, int limit_h) {
    for (int s = 1; s < 8; s *= 2) {
        if (w / s <= limit_w && h / s <= limit_h) {
            return s;
        }
    }
    return 8;
}

static int jpeg_option(int s) {
    return s == 2 ? JPEG_SCALE_HALF : s == 4 ? JPEG_SCALE_QUARTER : s == 8 ? JPEG_SCALE_EIGHTH : 0;
}

static bool decode_jpeg_still(uint8_t *data, size_t len, char *info, size_t info_n) {
    if (!s_jpeg->openRAM(data, (int)len, jpeg_draw_cb)) {
        snprintf(info, info_n, "Not a JPEG this decoder supports (error %d)", s_jpeg->getLastError());
        return false;
    }
    const int w = s_jpeg->getWidth(), h = s_jpeg->getHeight();
    const bool progressive = s_jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
    /* Decode at up to 2x screen size (sharper result after the final resample). Progressive JPEGs
     * are always decoded at 1/8 by the library (first scan only). */
    const int s = progressive ? 8 : jpeg_scale_for(w, h, 2 * k_scr_w, 2 * k_scr_h);
    const int tw = (w + s - 1) / s, th = (h + s - 1) / s;

    uint16_t *tmp = (uint16_t *)psram_alloc((size_t)tw * th * 2);
    if (!tmp) {
        s_jpeg->close();
        snprintf(info, info_n, "Out of memory (%dx%d)", tw, th);
        return false;
    }
    memset(tmp, 0, (size_t)tw * th * 2); /* scaled output can be 1 px short of the rounded-up size */
    JpegTarget t = {tmp, tw, tw, th};
    s_jpeg->setPixelType(RGB565_BIG_ENDIAN);
    s_jpeg->setUserPointer(&t);
    const uint32_t t0 = millis();
    const int ok = s_jpeg->decode(0, 0, progressive ? 0 : jpeg_option(s));
    s_jpeg->close();
    if (!ok) {
        heap_caps_free(tmp);
        snprintf(info, info_n, "JPEG decode failed (error %d)", s_jpeg->getLastError());
        return false;
    }
    int fw, fh;
    fit_size(tw, th, &fw, &fh);
    resample(tmp, tw, th, s_frame[s_back], fw, fh);
    heap_caps_free(tmp);
    present(fw, fh);
    snprintf(info, info_n, "%dx%d JPEG%s  -  %lu ms", w, h, progressive ? " (progressive: preview quality)" : "",
             (unsigned long)(millis() - t0));
    return true;
}

/* ---------- PNG ---------- */

struct PngTarget {
    uint16_t *dst;
    int sw, sh; /* source size */
    int dw, dh; /* fitted size */
    uint16_t *line;
};

static int png_draw_cb(PNGDRAW *d) {
    PngTarget *t = (PngTarget *)d->pUser;
    /* Destination rows whose nearest source row is this one. */
    const int y0 = (d->y * t->dh + t->sh - 1) / t->sh;
    const int y1 = ((d->y + 1) * t->dh + t->sh - 1) / t->sh;
    if (y0 >= y1) {
        return 1;
    }
    s_png->getLineAsRGB565(d, t->line, PNG_RGB565_BIG_ENDIAN, 0x000000);
    for (int y = y0; y < y1 && y < t->dh; y++) {
        uint16_t *out = t->dst + (size_t)y * t->dw;
        for (int x = 0; x < t->dw; x++) {
            out[x] = t->line[x * t->sw / t->dw];
        }
    }
    return 1;
}

static bool decode_png(uint8_t *data, size_t len, char *info, size_t info_n) {
    if (s_png->openRAM(data, (int)len, png_draw_cb) != PNG_SUCCESS) {
        snprintf(info, info_n, "Not a PNG this decoder supports (error %d)", s_png->getLastError());
        return false;
    }
    const int w = s_png->getWidth(), h = s_png->getHeight();
    if ((w * 4 + 1) * 2 > PNG_MAX_BUFFERED_PIXELS) {
        s_png->close();
        snprintf(info, info_n, "PNG too wide (%d px, max %d)", w, (PNG_MAX_BUFFERED_PIXELS / 2 - 1) / 4);
        return false;
    }
    PngTarget t;
    t.dst = s_frame[s_back];
    t.sw = w;
    t.sh = h;
    fit_size(w, h, &t.dw, &t.dh);
    t.line = (uint16_t *)psram_alloc((size_t)w * 2);
    if (!t.line) {
        s_png->close();
        snprintf(info, info_n, "Out of memory");
        return false;
    }
    const uint32_t t0 = millis();
    const int rc = s_png->decode(&t, 0);
    s_png->close();
    heap_caps_free(t.line);
    if (rc != PNG_SUCCESS) {
        snprintf(info, info_n, "PNG decode failed (error %d)", rc);
        return false;
    }
    present(t.dw, t.dh);
    snprintf(info, info_n, "%dx%d PNG  -  %lu ms", w, h, (unsigned long)(millis() - t0));
    return true;
}

/* ---------- BMP (uncompressed 16/24/32-bit) ---------- */

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static bool decode_bmp(uint8_t *data, size_t len, char *info, size_t info_n) {
    if (len < 54 || data[0] != 'B' || data[1] != 'M') {
        snprintf(info, info_n, "Not a BMP file");
        return false;
    }
    const uint32_t offset = rd32(data + 10);
    const int w = (int32_t)rd32(data + 18);
    int h = (int32_t)rd32(data + 22);
    const int bpp = rd16(data + 28);
    const uint32_t comp = rd32(data + 30);
    const bool top_down = h < 0;
    h = abs(h);
    if (w <= 0 || h <= 0 || !(bpp == 16 || bpp == 24 || bpp == 32) || !(comp == 0 || comp == 3)) {
        snprintf(info, info_n, "Unsupported BMP (%d-bit, compression %lu)", bpp, (unsigned long)comp);
        return false;
    }
    const size_t stride = ((size_t)w * bpp / 8 + 3) & ~3u;
    if (offset + stride * h > len) {
        snprintf(info, info_n, "Truncated BMP");
        return false;
    }
    int fw, fh;
    fit_size(w, h, &fw, &fh);
    uint16_t *dst = s_frame[s_back];
    for (int y = 0; y < fh; y++) {
        const int sy = y * h / fh;
        const uint8_t *row = data + offset + stride * (top_down ? sy : h - 1 - sy);
        for (int x = 0; x < fw; x++) {
            const int sx = x * w / fw;
            uint16_t px;
            if (bpp == 16) {
                px = swap16(rd16(row + sx * 2)); /* assume 565 bitfields (the common 16-bit layout) */
            } else {
                const uint8_t *p = row + sx * (bpp / 8);
                px = rgb565_be(p[2], p[1], p[0]); /* BGR(A) */
            }
            dst[(size_t)y * fw + x] = px;
        }
    }
    present(fw, fh);
    snprintf(info, info_n, "%dx%d BMP (%d-bit)", w, h, bpp);
    return true;
}

/* ---------- stills ---------- */

static void show_still(const char *path) {
    if (!ensure_decoders()) {
        ui_message("Out of memory");
        return;
    }
    ui_message("Loading...");
    char info[96] = "";
    File f = sd_fs().open(path, FILE_READ);
    if (!f) {
        ui_message("Cannot open file");
        return;
    }
    const size_t len = f.size();
    if (len == 0 || len > k_max_still_file) {
        f.close();
        ui_message(len ? "File too large (max 12 MB)" : "Empty file");
        return;
    }
    uint8_t *data = (uint8_t *)psram_alloc(len);
    if (!data) {
        f.close();
        ui_message("Out of memory");
        return;
    }
    const uint32_t t0 = millis();
    const size_t got = sd_read(f, data, len);
    f.close();
    const uint32_t t_read = millis() - t0;

    const String p(path);
    bool ok = false;
    if (got != len) {
        snprintf(info, sizeof(info), "Read error");
    } else if (ends_with_ci(p, ".png")) {
        ok = decode_png(data, len, info, sizeof(info));
    } else if (ends_with_ci(p, ".bmp")) {
        ok = decode_bmp(data, len, info, sizeof(info));
    } else {
        ok = decode_jpeg_still(data, len, info, sizeof(info));
    }
    heap_caps_free(data);

    serial_log("[media] %s: %s (read %lu ms)", base_name(path), info, (unsigned long)t_read);
    if (ok) {
        ui_info(info);
    } else {
        ui_message(info);
        ui_info("");
    }
}

/* ---------- MJPEG ---------- */

/** "clip_15fps.mjpeg" -> 15; otherwise the default. */
static int fps_from_name(const char *path) {
    const char *p = strcasestr(path, "fps");
    if (!p) {
        return k_default_fps;
    }
    const char *q = p;
    while (q > path && isdigit((unsigned char)q[-1])) {
        q--;
    }
    const int fps = atoi(q);
    return (fps >= 1 && fps <= 60) ? fps : k_default_fps;
}

/** Find the next complete JPEG (FFD8 ... FFD9) in buf[*pos, end). */
static bool next_frame(const uint8_t *buf, size_t end, size_t *pos, size_t *f_start, size_t *f_len) {
    size_t i = *pos;
    while (i + 1 < end && !(buf[i] == 0xFF && buf[i + 1] == 0xD8)) {
        i++;
    }
    if (i + 1 >= end) {
        *pos = i; /* keep a trailing 0xFF in case 0xD8 arrives with the next read */
        return false;
    }
    for (size_t j = i + 2; j + 1 < end; j++) {
        if (buf[j] == 0xFF && buf[j + 1] == 0xD9) {
            *f_start = i;
            *f_len = j + 2 - i;
            *pos = j + 2;
            return true;
        }
    }
    *pos = i; /* frame incomplete: resume from its start after more data arrives */
    return false;
}

/*
 * Playback is split in two so SD reads overlap decoding:
 *   reader task (core 0): reads the file in 32 KB chunks, cuts out complete JPEG frames and copies
 *     each into a free slot (k_slots of them) -> s_full_q.
 *   media task (core 1): takes a slot, decodes it into the hidden frame buffer, hands the slot back
 *     (s_free_q), then shows the frame.
 * Showing: with the controls hidden, the frame is sent straight to the panel
 * (lvgl_port_flush_frame), skipping LVGL's full-screen compose. With the controls visible it goes
 * through LVGL (lv_img + invalidate) so the overlay is drawn on top. Frames are letterboxed into
 * full-screen buffers, because the panel only takes full-frame updates.
 */
static const int k_slots = 4;
static const size_t k_slot_size = 192 * 1024;  /* largest frame accepted */
static const size_t k_stream_buf = 256 * 1024; /* read-ahead buffer (multiple frames) */
static const size_t k_read_chunk = 32 * 1024;  /* small reads: no long stalls */
static const uint8_t k_slot_end = 0xFF;        /* reader -> player: file has no frames */

static uint8_t *s_slot_data[k_slots];
static size_t s_slot_len[k_slots];
static uint8_t *s_stream = nullptr;
static QueueHandle_t s_free_q = nullptr;
static QueueHandle_t s_full_q = nullptr;
static TaskHandle_t s_reader = nullptr;
static SemaphoreHandle_t s_reader_done = nullptr;
static char s_reader_path[256];
static volatile bool s_reader_stop = false;
static volatile uint32_t s_read_bytes = 0; /* stats, reset by the player */
static volatile uint32_t s_read_ms = 0;

/* Seeking. Raw MJPEG has no index, so a seek jumps to a byte offset and resyncs on the next JPEG
 * start marker (FFD8 can't occur inside entropy-coded data, so this is reliable). Every queued
 * frame carries the seek generation it was read in; the player drops frames from older ones. */
static volatile uint32_t s_file_size = 0;
static volatile uint32_t s_seek_to = 0;
static volatile uint32_t s_seek_gen = 0; /* bumped by the player per seek */
static volatile bool s_seek_req = false;
static uint32_t s_slot_off[k_slots];      /* file offset of each queued frame */
static uint32_t s_slot_gen[k_slots];

/*
 * Frame index: the file offset of every frame. Raw MJPEG doesn't store a frame count, so the exact
 * length needs a full scan. The reader does it in the background, one 32 KB chunk at a time, only
 * while it is waiting for the player to free a slot (i.e. playback is ahead), so it never costs a
 * frame. When complete, the length and seeking become exact, and the index is cached on SD
 * (/.cache/mjpeg/<path>.idx, checked against the file size) so the next play has it at once.
 */
static const uint32_t k_index_max = 120000; /* ~83 min at 24 fps; 480 KB of PSRAM */
static const char *k_index_dir = "/.cache/mjpeg";
static const uint32_t k_index_magic = 0x31584A4D; /* "MJX1" */

static uint32_t *s_index = nullptr;
static volatile uint32_t s_index_count = 0;
static volatile bool s_index_done = false; /* s_index[0 .. count) complete and exact */
static File s_scan_f;
static uint8_t *s_scan_buf = nullptr;
static uint32_t s_scan_off = 0;
static bool s_scan_prev_ff = false;
static bool s_scan_active = false;
static uint32_t s_scan_t0 = 0;

/** "/mjpeg/clip.mjpeg" -> "/.cache/mjpeg/mjpeg_clip.mjpeg.idx" */
static String index_cache_path(const char *video) {
    String p = String(k_index_dir) + "/";
    for (const char *c = video[0] == '/' ? video + 1 : video; *c; c++) {
        p += (*c == '/') ? '_' : *c;
    }
    return p + ".idx";
}

static bool index_load(const char *video, uint32_t file_size) {
    File f = sd_fs().open(index_cache_path(video), FILE_READ);
    if (!f) {
        return false;
    }
    uint32_t hdr[3] = {0, 0, 0};
    bool ok = f.read((uint8_t *)hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == k_index_magic &&
              hdr[1] == file_size && hdr[2] > 0 && hdr[2] <= k_index_max &&
              sd_read(f, s_index, hdr[2] * 4) == hdr[2] * 4;
    f.close();
    if (ok) {
        s_index_count = hdr[2];
        s_index_done = true;
        serial_log("[media] index from cache: %lu frames", (unsigned long)hdr[2]);
    }
    return ok;
}

static void index_save(const char *video, uint32_t file_size) {
    sd_fs().mkdir("/.cache");
    sd_fs().mkdir(k_index_dir);
    File f = sd_fs().open(index_cache_path(video), FILE_WRITE);
    if (!f) {
        serial_log("[media] cannot write index cache");
        return;
    }
    const uint32_t hdr[3] = {k_index_magic, file_size, s_index_count};
    f.write((const uint8_t *)hdr, sizeof(hdr));
    f.write((const uint8_t *)s_index, s_index_count * 4);
    f.close();
}

/** Called by the reader when a file opens: use the cache, or start a background scan. */
static void index_begin(const char *video, uint32_t file_size) {
    s_index_done = false;
    s_index_count = 0;
    s_scan_active = false;
    if (!s_index || !s_scan_buf || index_load(video, file_size)) {
        return;
    }
    s_scan_f = sd_fs().open(video, FILE_READ); /* own handle: the playback read position is untouched */
    s_scan_off = 0;
    s_scan_prev_ff = false;
    s_scan_active = (bool)s_scan_f;
    s_scan_t0 = millis();
}

static void index_end(void) {
    if (s_scan_active) {
        s_scan_f.close();
        s_scan_active = false;
    }
}

/** Scan one chunk for frame starts (FF D8). Returns false when there is nothing left to do. */
static bool index_scan_step(const char *video, uint32_t file_size) {
    if (!s_scan_active) {
        return false;
    }
    const size_t n = sd_read(s_scan_f, s_scan_buf, k_read_chunk);
    for (size_t i = 0; i < n; i++) {
        if (s_scan_prev_ff && s_scan_buf[i] == 0xD8) {
            if (s_index_count >= k_index_max) {
                serial_log("[media] video too long to index (> %lu frames)", (unsigned long)k_index_max);
                index_end();
                return false;
            }
            s_index[s_index_count] = s_scan_off + (uint32_t)i - 1;
            s_index_count = s_index_count + 1;
        }
        s_scan_prev_ff = s_scan_buf[i] == 0xFF;
    }
    s_scan_off += n;
    if (n == 0) {
        index_end();
        if (s_index_count > 0) {
            s_index_done = true; /* after every offset is written: the player may now use them */
            index_save(video, file_size);
            serial_log("[media] indexed %lu frames in %lu s", (unsigned long)s_index_count,
                       (unsigned long)((millis() - s_scan_t0) / 1000));
        }
        return false;
    }
    return true;
}

static volatile bool s_overlay_on = true; /* written by the UI */
static volatile bool s_video_paused = false; /* player -> reader: index at full speed only while paused */
static lv_img_dsc_t s_video_dsc;           /* lv_img source while a video plays */

static void reader_task(void *arg) {
    LV_UNUSED(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        File f = sd_fs().open(s_reader_path, FILE_READ);
        s_file_size = f ? (uint32_t)f.size() : 0;
        if (f) {
            index_begin(s_reader_path, s_file_size);
        }
        size_t end = 0, pos = 0, fs = 0, fl = 0;
        uint32_t base = 0; /* file offset of s_stream[0] */
        uint32_t gen = s_seek_gen;
        bool have = false, any = false;
        while (f && !s_reader_stop) {
            if (s_seek_req) {
                s_seek_req = false;
                gen = s_seek_gen;
                base = s_seek_to;
                f.seek(base);
                end = pos = 0;
                have = false;
            }
            if (!have) {
                have = next_frame(s_stream, end, &pos, &fs, &fl);
                if (!have) {
                    /* Drop consumed bytes, then read one chunk. */
                    if (pos > 0) {
                        memmove(s_stream, s_stream + pos, end - pos);
                        end -= pos;
                        base += pos;
                        pos = 0;
                    }
                    if (end == k_stream_buf) {
                        serial_log("[media] frame over %u KB, skipped", (unsigned)(k_stream_buf / 1024));
                        base += end;
                        end = 0;
                    }
                    const size_t want = (k_stream_buf - end) < k_read_chunk ? (k_stream_buf - end) : k_read_chunk;
                    const uint32_t t0 = millis();
                    const size_t n = sd_read(f, s_stream + end, want);
                    s_read_ms += millis() - t0;
                    s_read_bytes += n;
                    /* Same watchdog rule as indexing: if the card is the bottleneck this loop never
                     * waits on a queue, so give the core-0 idle task a moment after every chunk. */
                    vTaskDelay(1);
                    if (n == 0) {
                        if (!any) {
                            const uint8_t m = k_slot_end;
                            xQueueSend(s_full_q, &m, portMAX_DELAY);
                            break;
                        }
                        f.seek(0); /* loop */
                        end = pos = 0;
                        base = 0;
                    }
                    end += n;
                    continue;
                }
            }
            if (fl > k_slot_size) {
                have = false;
                continue;
            }
            uint8_t slot;
            if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) {
                /* Playback is ahead (all slots full): use the time to index the file. */
                if (index_scan_step(s_reader_path, s_file_size)) {
                    /*
                     * Must sleep between chunks. Arduino SD reads busy-wait on SPI, so back-to-back
                     * chunks kept this task (core 0, prio 2) running for seconds while paused, the
                     * core-0 idle task never ran, and the 5 s task watchdog reset the chip mid-read -
                     * which also left the SD card stuck so the next boot couldn't mount it.
                     */
                    /* While playing, only one chunk per 100 ms: full-speed counting kept core 0 busy
                     * (SD reads busy-wait) and starved the UI. Full speed while paused. */
                    vTaskDelay(pdMS_TO_TICKS(s_video_paused ? 2 : 100));
                    continue;
                }
                if (xQueueReceive(s_free_q, &slot, pdMS_TO_TICKS(50)) != pdTRUE) {
                    continue; /* still full (or paused): re-check stop / seek, keep the frame */
                }
            }
            if (s_seek_req) {
                xQueueSend(s_free_q, &slot, 0); /* a seek arrived meanwhile: this frame is stale */
                have = false;
                continue;
            }
            memcpy(s_slot_data[slot], s_stream + fs, fl);
            s_slot_len[slot] = fl;
            s_slot_off[slot] = base + (uint32_t)fs;
            s_slot_gen[slot] = gen;
            xQueueSend(s_full_q, &slot, portMAX_DELAY); /* never blocks: queue holds every slot */
            have = false;
            any = true;
        }
        index_end(); /* an unfinished scan is dropped (and redone next time) */
        if (f) {
            f.close();
        } else {
            const uint8_t m = k_slot_end;
            xQueueSend(s_full_q, &m, portMAX_DELAY);
        }
        xSemaphoreGive(s_reader_done);
    }
}

static bool video_begin(void) {
    if (s_reader) {
        return true;
    }
    s_stream = (uint8_t *)psram_alloc(k_stream_buf);
    for (int i = 0; i < k_slots; i++) {
        s_slot_data[i] = (uint8_t *)psram_alloc(k_slot_size);
        if (!s_slot_data[i]) {
            return false;
        }
    }
    if (!s_stream) {
        return false;
    }
    /* Index is optional: without it the length stays an estimate. */
    s_index = (uint32_t *)psram_alloc(k_index_max * sizeof(uint32_t));
    s_scan_buf = (uint8_t *)psram_alloc(k_read_chunk);
    s_free_q = xQueueCreate(k_slots, sizeof(uint8_t));
    s_full_q = xQueueCreate(k_slots + 1, sizeof(uint8_t));
    s_reader_done = xSemaphoreCreateBinary();
    /* Core 0: LVGL is nearly idle there while the direct path plays; priority below LVGL. */
    xTaskCreatePinnedToCoreWithCaps(reader_task, "media_rd", 6144, nullptr, 2, &s_reader, 0, MALLOC_CAP_SPIRAM); /* PSRAM stack: never touch flash (NVS etc.) here */
    return true;
}

/** Show the frame in s_frame[s_back]: direct to the panel, or through LVGL if the overlay is up. */
static uint32_t show_video_frame(void) {
    const uint32_t t0 = millis();
    bsp_display_lock(0);
    if (!s_img) { /* screen already released */
        bsp_display_unlock();
        return 0;
    }
    s_video_dsc.data = (const uint8_t *)s_frame[s_back];
    if (s_overlay_on || lv_scr_act() != s_scr) {
        lv_obj_invalidate(s_img);
    } else {
        lvgl_port_flush_frame(lv_disp_get_default(), (const lv_color_t *)s_frame[s_back]);
    }
    bsp_display_unlock();
    s_back ^= 1;
    return millis() - t0;
}

static void fmt_time(char *buf, size_t n, uint32_t s) {
    if (s >= 3600) {
        snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    } else {
        snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

/**
 * Progress bar + "elapsed / total" (task side). Leaves the bar alone while the user drags it.
 * An estimated total is shown as "~4:56" until the background index makes it exact.
 */
static void ui_progress(int permille, uint32_t elapsed_s, uint32_t total_s, bool exact) {
    s_total_s = total_s;
    s_total_exact = exact;
    char a[16], b[16], buf[40];
    fmt_time(a, sizeof(a), elapsed_s);
    fmt_time(b, sizeof(b), total_s);
    snprintf(buf, sizeof(buf), "%s / %s%s", a, exact ? "" : "~", b);
    bsp_display_lock(0);
    if (s_seek_bar && !lv_obj_has_state(s_seek_bar, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_seek_bar, permille, LV_ANIM_OFF);
        lv_label_set_text(s_time_lbl, buf);
    }
    bsp_display_unlock();
}

/** Play until a new command arrives (left in the queue for media_task). Loops at end of file. */
static void play_video(const char *path) {
    if (!ensure_decoders() || !video_begin()) {
        ui_message("Out of memory");
        return;
    }
    /* Fresh pipeline: all slots free, reader started on this file. */
    xQueueReset(s_free_q);
    xQueueReset(s_full_q);
    for (uint8_t i = 0; i < k_slots; i++) {
        xQueueSend(s_free_q, &i, 0);
    }
    strlcpy(s_reader_path, path, sizeof(s_reader_path));
    s_reader_stop = false;
    s_video_paused = false;
    s_read_bytes = s_read_ms = 0;
    s_index_done = false; /* the reader re-checks the cache / rescans for this file */
    s_index_count = 0;
    xTaskNotifyGive(s_reader);

    /* Black letterbox; lv_img shows the full-screen buffer. */
    memset(s_frame[0], 0, (size_t)k_scr_w * k_scr_h * 2);
    memset(s_frame[1], 0, (size_t)k_scr_w * k_scr_h * 2);
    bsp_display_lock(0);
    memset(&s_video_dsc, 0, sizeof(s_video_dsc));
    s_video_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_video_dsc.header.w = k_scr_w;
    s_video_dsc.header.h = k_scr_h;
    s_video_dsc.data_size = (uint32_t)k_scr_w * k_scr_h * 2;
    s_video_dsc.data = (const uint8_t *)s_frame[0];
    lv_img_set_src(s_img, &s_video_dsc);
    lv_obj_center(s_img);
    bsp_display_unlock();

    const int fps = fps_from_name(path);
    const uint32_t frame_ms = 1000 / fps;
    int scale = 0, src_w = 0, src_h = 0, vw = 0, vh = 0, ox = 0, oy = 0;
    bool paused = false, first = true;
    bool show_one = false; /* paused + seek: still show the frame at the new position */
    uint32_t frames = 0, decode_ms = 0, show_ms = 0, frame_bytes = 0, starved = 0;
    uint32_t stat_t0 = millis(), log_t0 = millis(), next_due = millis(), progress_t0 = 0;
    /*
     * Time display. Raw MJPEG has no timestamps, so:
     *  - elapsed counts the frames actually played (exact, always moves forward); only a seek
     *    re-estimates it once from the landing offset, and a loop back to the start resets it;
     *  - total = file size / average frame size over EVERY frame played so far. (A moving average
     *    of recent frames swung with scene content - busy scenes have bigger frames - which made
     *    both numbers jump around.)
     */
    uint32_t cur_off = 0, last_off = 0;
    uint64_t bytes_seen = 0;
    uint32_t frames_seen = 0;
    uint32_t frame_no = 0;      /* index of the frame on screen */
    bool need_anchor = false;   /* set by a seek */
    int32_t anchor_frame = -1;  /* exact frame of the seek target (index known), else -1 */
    bool have_frame = false;
    uint32_t shown_total_s = 0; /* only changed when the estimate moves by 2 s or more */
    serial_log("[media] playing %s at up to %d fps", base_name(path), fps);

    for (;;) {
        MediaCmd c;
        if (xQueuePeek(s_queue, &c, paused ? pdMS_TO_TICKS(50) : 0) == pdTRUE) {
            if (c.cmd == Cmd::TogglePause) {
                xQueueReceive(s_queue, &c, 0);
                paused = !paused;
                s_video_paused = paused;
                ui_info(paused ? "Paused" : "Playing");
                next_due = millis();
                continue;
            }
            if (c.cmd == Cmd::Seek) {
                xQueueReceive(s_queue, &c, 0);
                const uint32_t permille = (uint32_t)constrain(c.arg, 0, 1000);
                if (s_index_done) {
                    /* Exact: the bar is time-based, so jump to that frame's offset. */
                    const uint32_t n = s_index_count;
                    uint32_t target = (uint32_t)((uint64_t)n * permille / 1000);
                    if (target >= n) {
                        target = n - 1;
                    }
                    cur_off = s_index[target];
                    anchor_frame = (int32_t)target;
                } else {
                    cur_off = (uint32_t)((uint64_t)s_file_size * permille / 1000);
                    anchor_frame = -1; /* estimate the frame number from the landing offset */
                }
                s_seek_to = cur_off;
                s_seek_gen = s_seek_gen + 1;
                s_seek_req = true; /* reader jumps there; frames from before are dropped below */
                show_one = paused;
                need_anchor = true;
                next_due = millis();
                progress_t0 = 0; /* refresh the bar as soon as the new frame shows */
                continue;
            }
            break; /* Stop / another file: handled by media_task */
        }
        if (paused && !show_one) {
            continue;
        }

        uint8_t slot;
        if (xQueueReceive(s_full_q, &slot, pdMS_TO_TICKS(100)) != pdTRUE) {
            starved++;
            continue;
        }
        if (slot == k_slot_end) {
            ui_message("No MJPEG frames found");
            break;
        }
        if (s_slot_gen[slot] != s_seek_gen) {
            xQueueSend(s_free_q, &slot, 0); /* read before the last seek: discard */
            continue;
        }
        cur_off = s_slot_off[slot];
        bytes_seen += s_slot_len[slot];
        frames_seen++;
        if (need_anchor) {
            frame_no = anchor_frame >= 0 ? (uint32_t)anchor_frame
                                         : (uint32_t)(cur_off / ((float)bytes_seen / frames_seen));
            need_anchor = false;
        } else if (!have_frame || cur_off < last_off) {
            frame_no = 0; /* first frame, or looped back to the start */
        } else {
            frame_no++;
        }
        have_frame = true;
        last_off = cur_off;

        const uint32_t t0 = millis();
        const bool opened = s_jpeg->openRAM(s_slot_data[slot], (int)s_slot_len[slot], jpeg_draw_cb);
        if (opened && scale == 0) {
            src_w = s_jpeg->getWidth();
            src_h = s_jpeg->getHeight();
            scale = jpeg_scale_for(src_w, src_h, k_scr_w, k_scr_h);
            vw = (src_w + scale - 1) / scale;
            vh = (src_h + scale - 1) / scale;
            if (vw > k_scr_w) vw = k_scr_w; /* odd sizes: crop the overflow */
            if (vh > k_scr_h) vh = k_scr_h;
            ox = (k_scr_w - vw) / 2;
            oy = (k_scr_h - vh) / 2;
        }
        int ok = 0;
        if (opened) {
            JpegTarget t = {s_frame[s_back] + (size_t)oy * k_scr_w + ox, k_scr_w, vw, vh};
            s_jpeg->setPixelType(RGB565_BIG_ENDIAN);
            s_jpeg->setUserPointer(&t);
            ok = s_jpeg->decode(0, 0, jpeg_option(scale));
            s_jpeg->close();
        }
        frame_bytes += s_slot_len[slot];
        xQueueSend(s_free_q, &slot, 0); /* data consumed: let the reader refill it */
        decode_ms += millis() - t0;
        if (!ok) {
            continue; /* corrupt frame: skip */
        }

        const int32_t wait = (int32_t)(next_due - millis());
        if (wait > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
        next_due = millis() + frame_ms;
        if (first) {
            first = false;
            ui_message("");
        }
        show_ms += show_video_frame();
        frames++;
        show_one = false;

        const uint32_t now = millis();
        /* Progress: only drawn with the controls up (direct-to-panel frames would hide it anyway). */
        if (s_overlay_on && (progress_t0 == 0 || now - progress_t0 >= 250)) {
            progress_t0 = now;
            const uint32_t size = s_file_size;
            const uint32_t elapsed_s = frame_no / fps;
            if (s_index_done) {
                /* Exact length from the frame index; the bar becomes time-based (matches seeking). */
                const uint32_t n = s_index_count;
                const uint32_t total_s = (n + fps - 1) / fps;
                ui_progress((int)((uint64_t)frame_no * 1000 / n), elapsed_s, total_s, true);
            } else {
                const float avg = (float)bytes_seen / frames_seen;
                const uint32_t total_s = (uint32_t)(size / avg / fps);
                if (shown_total_s == 0 || abs((int)total_s - (int)shown_total_s) >= 2) {
                    shown_total_s = total_s;
                }
                if (elapsed_s > shown_total_s) {
                    shown_total_s = elapsed_s; /* never show elapsed past the end */
                }
                /* Bar: byte position (what seeking uses without an index); always moves forward. */
                ui_progress(size ? (int)((uint64_t)cur_off * 1000 / size) : 0, elapsed_s, shown_total_s, false);
            }
        }
        if (now - stat_t0 >= 1000 && s_overlay_on) {
            char info[64];
            snprintf(info, sizeof(info), "%dx%d  -  %.1f fps", src_w, src_h, frames * 1000.0f / (now - log_t0));
            ui_info(info);
            stat_t0 = now;
        }
        if (now - log_t0 >= 5000) {
            const uint32_t n = frames ? frames : 1;
            serial_log("[media] %.1f fps | decode %lu ms, show %lu ms | frame %lu KB, SD %lu KB/s | waited for SD %lux",
                       frames * 1000.0f / (now - log_t0), (unsigned long)(decode_ms / n), (unsigned long)(show_ms / n),
                       (unsigned long)(frame_bytes / n / 1024),
                       (unsigned long)(s_read_ms ? s_read_bytes / s_read_ms : 0), (unsigned long)starved);
            frames = decode_ms = show_ms = frame_bytes = starved = 0;
            s_read_bytes = s_read_ms = 0;
            log_t0 = now;
        }
    }

    /* Stop the reader and wait until it has closed the file. */
    s_reader_stop = true;
    xSemaphoreTake(s_reader_done, portMAX_DELAY);
}

/* ---------- task ---------- */

/** Frame buffers + decoders, allocated by the media task on first use after a release. */
static bool ensure_decoders(void) {
    if (s_jpeg && s_png && s_frame[0] && s_frame[1]) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        if (!s_frame[i]) {
            s_frame[i] = (uint16_t *)psram_alloc((size_t)k_scr_w * k_scr_h * 2);
        }
    }
    if (!s_jpeg) {
        /* Internal RAM: JPEGDEC's tables are hit constantly while decoding (faster than PSRAM). */
        void *jm = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!jm) {
            jm = psram_alloc(sizeof(JPEGDEC));
        }
        s_jpeg = jm ? new (jm) JPEGDEC() : nullptr;
    }
    if (!s_png) {
        void *pm = psram_alloc(sizeof(PNG)); /* large (4096 px line buffer): PSRAM */
        s_png = pm ? new (pm) PNG() : nullptr;
    }
    if (!s_jpeg || !s_png || !s_frame[0] || !s_frame[1]) {
        serial_log("[media] out of memory");
        return false;
    }
    serial_log("[media] buffers allocated (JPEGDEC %u B, PNG %u B)", (unsigned)sizeof(JPEGDEC),
               (unsigned)sizeof(PNG));
    return true;
}

/**
 * Leaving the viewer: free everything big (frame buffers, decoders, read-ahead slots, frame index,
 * the reader task). Runs on the media task, so nothing is mid-decode; the UI deletes the screen
 * once s_released is set.
 */
static void media_release_buffers(void) {
    if (s_reader) {
        vTaskDeleteWithCaps(s_reader); /* idle: play_video() waited for it to close its file */
        s_reader = nullptr;
        vQueueDelete(s_free_q);
        vQueueDelete(s_full_q);
        vSemaphoreDelete(s_reader_done);
        s_free_q = s_full_q = nullptr;
        s_reader_done = nullptr;
    }
    auto release = [](void *p) {
        if (p) {
            heap_caps_free(p);
        }
    };
    release(s_stream);
    s_stream = nullptr;
    for (int i = 0; i < k_slots; i++) {
        release(s_slot_data[i]);
        s_slot_data[i] = nullptr;
    }
    release(s_index);
    s_index = nullptr;
    release(s_scan_buf);
    s_scan_buf = nullptr;
    if (s_jpeg) {
        s_jpeg->~JPEGDEC();
        heap_caps_free(s_jpeg);
        s_jpeg = nullptr;
    }
    if (s_png) {
        s_png->~PNG();
        heap_caps_free(s_png);
        s_png = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        release(s_frame[i]);
        s_frame[i] = nullptr;
    }
    s_released = true;
    serial_log("[media] released buffers");
}

static void media_task(void *arg) {
    LV_UNUSED(arg);
    MediaCmd c;
    for (;;) {
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Several queued requests (fast swiping): only the newest matters. */
        MediaCmd newer;
        while (xQueueReceive(s_queue, &newer, 0) == pdTRUE) {
            c = newer;
        }
        switch (c.cmd) {
        case Cmd::ShowImage:
            show_still(c.path);
            break;
        case Cmd::PlayVideo:
            play_video(c.path);
            break;
        case Cmd::Release:
            media_release_buffers();
            break;
        case Cmd::TogglePause:
        case Cmd::Seek:
        case Cmd::Stop:
            break;
        }
    }
}

/** The media task and its command queue live for the whole run (12 KB stack); buffers don't. */
static bool media_begin(void) {
    if (s_task) {
        return true;
    }
    s_queue = xQueueCreate(4, sizeof(MediaCmd));
    /* Core 1 at loop()'s priority (1): a higher one let back-to-back decoding starve loop() (WiFi
     * housekeeping). loop() sleeps 5 ms per pass, so decoding still gets nearly all of core 1. */
    xTaskCreatePinnedToCoreWithCaps(media_task, "media", 12288, nullptr, 1, &s_task, 1, MALLOC_CAP_SPIRAM); /* PSRAM stack: never touch flash (NVS etc.) here */
    return s_task != nullptr;
}

static void send(Cmd cmd, const char *path, int arg = 0) {
    MediaCmd c;
    c.cmd = cmd;
    c.arg = arg;
    strlcpy(c.path, path ? path : "", sizeof(c.path));
    if (xQueueSend(s_queue, &c, 0) != pdTRUE) {
        /* Queue full: drop the oldest so the latest request still gets through. */
        MediaCmd old;
        xQueueReceive(s_queue, &old, 0);
        xQueueSend(s_queue, &c, 0);
    }
}

/* ---------- UI ---------- */

static void set_overlay_visible(bool show) {
    s_overlay_on = show; /* player: direct-to-panel only while the controls are hidden */
    auto apply = [show](lv_obj_t *o) {
        if (!o) {
            return;
        }
        if (show) {
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    };
    apply(s_overlay);
    apply(s_video_mode ? nullptr : s_prev_btn);
    apply(s_video_mode ? nullptr : s_next_btn);
    apply(s_video_mode ? s_bottom : nullptr);
    if (show) {
        lv_timer_reset(s_hide_timer);
        lv_timer_resume(s_hide_timer);
    }
}

static void hide_timer_cb(lv_timer_t *t) {
    lv_timer_pause(t);
    set_overlay_visible(false);
}

/**
 * Overlay title. The built-in Montserrat font only covers Latin, so names with Chinese (or any
 * other non-ASCII) characters use the SD UI font, as the Files list does.
 */
static void set_title(const char *text) {
    bool ascii = true;
    for (const char *p = text; *p; p++) {
        if ((unsigned char)*p >= 0x80) {
            ascii = false;
            break;
        }
    }
    lv_obj_set_style_text_font(s_title, ascii ? &lv_font_montserrat_16 : ui_font_get(), 0);
    lv_label_set_text(s_title, text);
}

static void show_image_at(size_t i) {
    s_image_i = i;
    char title[300];
    snprintf(title, sizeof(title), "%s  (%u/%u)", base_name(s_images[i].c_str()), (unsigned)(i + 1),
             (unsigned)s_images.size());
    set_title(title);
    lv_label_set_text(s_info, "");
    send(Cmd::ShowImage, s_images[i].c_str());
}

static void step_image(int delta) {
    if (s_video_mode || s_images.size() < 2) {
        return;
    }
    const size_t n = s_images.size();
    show_image_at((s_image_i + n + delta) % n);
}

/** Once the media task has freed its buffers, delete the viewer screen (if not reopened). */
static void reaper_cb(lv_timer_t *t) {
    if (!s_released || lv_scr_act() == s_scr) {
        return;
    }
    lv_timer_del(t);
    s_reaper = nullptr;
    lv_timer_del(s_hide_timer);
    lv_obj_del(s_scr);
    s_scr = s_img = s_overlay = s_title = s_info = s_msg = nullptr;
    s_prev_btn = s_next_btn = s_bottom = s_play_btn = s_play_lbl = s_seek_bar = s_time_lbl = nullptr;
    s_hide_timer = nullptr;
    s_return_scr = nullptr;
    s_images.clear();
    s_images.shrink_to_fit();
    serial_log("[media] viewer screen released");
}

static void back_cb(lv_event_t *e) {
    LV_UNUSED(e);
    /* Release = stop + free every buffer; the screen goes once that is done (reaper_cb). */
    s_released = false;
    send(Cmd::Release, nullptr);
    if (s_return_scr) {
        lv_scr_load(s_return_scr);
    }
    if (!s_reaper) {
        s_reaper = lv_timer_create(reaper_cb, 100, nullptr);
    }
}

static void prev_cb(lv_event_t *e) {
    LV_UNUSED(e);
    step_image(-1);
    set_overlay_visible(true);
}

static void next_cb(lv_event_t *e) {
    LV_UNUSED(e);
    step_image(+1);
    set_overlay_visible(true);
}

static void play_cb(lv_event_t *e) {
    LV_UNUSED(e);
    send(Cmd::TogglePause, nullptr);
    const bool was_play = strcmp(lv_label_get_text(s_play_lbl), LV_SYMBOL_PAUSE) == 0;
    lv_label_set_text(s_play_lbl, was_play ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    set_overlay_visible(true);
}

/**
 * Progress bar: dragging previews the target time and keeps the controls up; releasing seeks.
 * The player doesn't move the bar while it's pressed (see ui_progress).
 */
static void seek_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int v = lv_slider_get_value(s_seek_bar);
    if (code == LV_EVENT_PRESSED) {
        lv_timer_pause(s_hide_timer);
    } else if (code == LV_EVENT_VALUE_CHANGED && lv_obj_has_state(s_seek_bar, LV_STATE_PRESSED)) {
        char a[16], b[16];
        fmt_time(a, sizeof(a), (uint32_t)((uint64_t)s_total_s * v / 1000));
        fmt_time(b, sizeof(b), s_total_s);
        lv_label_set_text_fmt(s_time_lbl, "%s / %s%s", a, s_total_exact ? "" : "~", b);
    } else if (code == LV_EVENT_RELEASED) {
        send(Cmd::Seek, nullptr, v);
        set_overlay_visible(true); /* restarts the auto-hide countdown */
    }
}

static void screen_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        if (s_video_mode) {
            return; /* swipes are for stepping through images only */
        }
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            step_image(+1);
        } else if (dir == LV_DIR_RIGHT) {
            step_image(-1);
        }
        lv_indev_wait_release(lv_indev_get_act()); /* don't also count it as a tap */
    } else if (code == LV_EVENT_CLICKED) {
        set_overlay_visible(lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN));
    }
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, lv_obj_t **label_out) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (label_out) {
        *label_out = l;
    }
    return b;
}

static void build_screen(void) {
    if (s_scr) {
        return;
    }
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_CLICKED, nullptr);

    s_img = lv_img_create(s_scr);
    lv_obj_center(s_img);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    s_msg = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_white(), 0);
    lv_obj_center(s_msg);

    /* Top overlay: back | title | info */
    s_overlay = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), 48);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_overlay, 8, 0);
    lv_obj_set_style_pad_column(s_overlay, 10, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 44, 36);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);

    s_title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);

    s_info = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_info, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

    s_prev_btn = round_btn(s_scr, LV_SYMBOL_LEFT, prev_cb, nullptr);
    lv_obj_align(s_prev_btn, LV_ALIGN_LEFT_MID, 10, 20);
    s_next_btn = round_btn(s_scr, LV_SYMBOL_RIGHT, next_cb, nullptr);
    lv_obj_align(s_next_btn, LV_ALIGN_RIGHT_MID, -10, 20);
    /* Bottom bar (video): play/pause | progress (drag to seek) | elapsed / total */
    s_bottom = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_bottom);
    lv_obj_set_size(s_bottom, LV_PCT(100), 56);
    lv_obj_align(s_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bottom, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_bottom, 10, 0);
    lv_obj_set_style_pad_column(s_bottom, 16, 0);
    lv_obj_set_flex_flow(s_bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_bottom, LV_OBJ_FLAG_SCROLLABLE);

    s_play_btn = round_btn(s_bottom, LV_SYMBOL_PAUSE, play_cb, &s_play_lbl);
    lv_obj_set_size(s_play_btn, 44, 44);

    s_seek_bar = lv_slider_create(s_bottom);
    lv_slider_set_range(s_seek_bar, 0, 1000);
    lv_obj_set_height(s_seek_bar, 6);
    lv_obj_set_flex_grow(s_seek_bar, 1);
    lv_obj_set_style_bg_color(s_seek_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_seek_bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_seek_bar, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_seek_bar, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_seek_bar, 6, LV_PART_KNOB);
    /* Generous touch area around the thin bar */
    lv_obj_set_ext_click_area(s_seek_bar, 16);
    lv_obj_clear_flag(s_seek_bar, LV_OBJ_FLAG_GESTURE_BUBBLE); /* a fast drag must not count as a swipe */
    lv_obj_clear_flag(s_bottom, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_seek_bar, seek_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_seek_bar, seek_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_seek_bar, seek_cb, LV_EVENT_RELEASED, nullptr);

    s_time_lbl = lv_label_create(s_bottom);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_white(), 0);
    lv_label_set_text(s_time_lbl, "0:00 / 0:00");

    s_hide_timer = lv_timer_create(hide_timer_cb, k_overlay_hide_ms, nullptr);
    lv_timer_pause(s_hide_timer);
}

static void enter(bool video) {
    if (s_reaper) { /* reopened before the old screen was released: keep using it */
        lv_timer_del(s_reaper);
        s_reaper = nullptr;
    }
    build_screen();
    s_video_mode = video;
    if (lv_scr_act() != s_scr) {
        s_return_scr = lv_scr_act();
    }
    lv_img_set_src(s_img, nullptr); /* don't flash the previous picture */
    lv_label_set_text(s_msg, "Loading...");
    lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_prev_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bottom, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(s_scr);
    set_overlay_visible(true);
}

void media_ui_open_images(const std::vector<String> &paths, size_t index) {
    if (paths.empty() || !media_begin()) {
        return;
    }
    s_images = paths;
    enter(false);
    show_image_at(index < paths.size() ? index : 0);
}

void media_ui_open_video(const String &path) {
    if (!media_begin()) {
        return;
    }
    enter(true);
    set_title(base_name(path.c_str()));
    lv_label_set_text(s_info, "");
    lv_slider_set_value(s_seek_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(s_time_lbl, "0:00 / --:--");
    s_total_s = 0;
    lv_label_set_text(s_play_lbl, LV_SYMBOL_PAUSE);
    send(Cmd::PlayVideo, path.c_str());
}
