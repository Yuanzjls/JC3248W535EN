#include "media_ui.h"

#include <lvgl.h> /* before PNGdec: see the #undef below */

#include <JPEGDEC.h>
#include <PNGdec.h>
/* PNGdec's bundled zlib does `#define local static` in a public header. LVGL (lv_meter.h) has a
 * parameter named `local`, so keep the macro from leaking into anything included later. */
#undef local

#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <new>
#include <strings.h>

#include "esp_bsp.h"
#include "serial_log.h"
#include "ui_theme.h"

/*
 * Image viewer (JPEG / PNG / BMP) and raw-MJPEG player.
 *
 * Decoding runs in a "media" task on core 1 (core 0 is busy with LVGL redraws). Frames go into one
 * of two RGB565 buffers in PSRAM; the task decodes into the hidden one and then, under the LVGL
 * lock, points the lv_img at it. LVGL only reads a buffer while rendering under that same lock, so
 * the buffer being decoded into is never on screen: no tearing.
 *
 * Pixels are big-endian RGB565 because lv_conf.h has LV_COLOR_16_SWAP=1.
 */

static const int k_scr_w = 480;
static const int k_scr_h = 320;
static const size_t k_max_still_file = 12 * 1024 * 1024; /* bigger images are refused */
static const size_t k_video_buf = 512 * 1024;            /* holds several MJPEG frames */
static const int k_default_fps = 24;                     /* raw MJPEG has no timing; "_15fps" in the name overrides */
static const uint32_t k_overlay_hide_ms = 3000;

/* ---------- shared state ---------- */

enum class Cmd { ShowImage, PlayVideo, TogglePause, Stop };
struct MediaCmd {
    Cmd cmd;
    char path[256];
};

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_task = nullptr;

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
static lv_obj_t *s_play_btn = nullptr;
static lv_obj_t *s_play_lbl = nullptr;
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
    uint16_t *buf;
    int w, h; /* buffer size; blocks are clipped to it */
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
            memcpy(t->buf + (size_t)y * t->w + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)w * 2);
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
    JpegTarget t = {tmp, tw, th};
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
    ui_message("Loading...");
    char info[96] = "";
    File f = SD.open(path, FILE_READ);
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
    const size_t got = f.read(data, len);
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

/** Play until a new command arrives (left in the queue for media_task). Loops at end of file. */
static void play_video(const char *path) {
    ui_message("Loading...");
    File f = SD.open(path, FILE_READ);
    if (!f) {
        ui_message("Cannot open file");
        return;
    }
    uint8_t *buf = (uint8_t *)psram_alloc(k_video_buf);
    if (!buf) {
        f.close();
        ui_message("Out of memory");
        return;
    }
    const int fps = fps_from_name(path);
    const uint32_t frame_ms = 1000 / fps;
    size_t end = 0, pos = 0;
    int scale = 0, vw = 0, vh = 0, src_w = 0, src_h = 0;
    bool paused = false;
    uint32_t frames = 0, stat_t0 = millis(), next_due = millis();
    uint32_t decode_ms = 0;
    serial_log("[media] playing %s at up to %d fps", base_name(path), fps);

    for (;;) {
        /* Commands: pause toggles here; anything else ends playback (and is handled by the task). */
        MediaCmd c;
        if (xQueuePeek(s_queue, &c, paused ? pdMS_TO_TICKS(50) : 0) == pdTRUE) {
            if (c.cmd == Cmd::TogglePause) {
                xQueueReceive(s_queue, &c, 0);
                paused = !paused;
                ui_info(paused ? "Paused" : "Playing");
                next_due = millis();
                continue;
            }
            break;
        }
        if (paused) {
            continue;
        }

        size_t fs = 0, fl = 0;
        if (!next_frame(buf, end, &pos, &fs, &fl)) {
            /* Need more data: drop consumed bytes, then read. */
            if (pos > 0) {
                memmove(buf, buf + pos, end - pos);
                end -= pos;
                pos = 0;
            }
            if (end == k_video_buf) {
                serial_log("[media] frame larger than %u KB, skipping", (unsigned)(k_video_buf / 1024));
                end = 0;
            }
            const size_t n = f.read(buf + end, k_video_buf - end);
            if (n == 0) {
                if (frames == 0 && src_w == 0) {
                    ui_message("No MJPEG frames found");
                    break;
                }
                f.seek(0); /* loop */
                end = pos = 0;
            }
            end += n;
            continue;
        }

        const uint32_t t0 = millis();
        if (!s_jpeg->openRAM(buf + fs, (int)fl, jpeg_draw_cb)) {
            continue; /* corrupt frame: skip */
        }
        if (scale == 0) {
            src_w = s_jpeg->getWidth();
            src_h = s_jpeg->getHeight();
            scale = jpeg_scale_for(src_w, src_h, k_scr_w, k_scr_h);
            vw = (src_w + scale - 1) / scale;
            vh = (src_h + scale - 1) / scale;
            if (vw > k_scr_w) vw = k_scr_w; /* odd sizes: crop the overflow */
            if (vh > k_scr_h) vh = k_scr_h;
        }
        JpegTarget t = {s_frame[s_back], vw, vh};
        s_jpeg->setPixelType(RGB565_BIG_ENDIAN);
        s_jpeg->setUserPointer(&t);
        const int ok = s_jpeg->decode(0, 0, jpeg_option(scale));
        s_jpeg->close();
        decode_ms += millis() - t0;
        if (!ok) {
            continue;
        }

        /* Pace to the target frame rate (if decoding is fast enough to need it). */
        const int32_t wait = (int32_t)(next_due - millis());
        if (wait > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
        next_due = millis() + frame_ms;
        present(vw, vh);
        frames++;

        const uint32_t now = millis();
        if (now - stat_t0 >= 1000) {
            char info[64];
            snprintf(info, sizeof(info), "%dx%d  -  %.1f fps  (decode %lu ms)", src_w, src_h,
                     frames * 1000.0f / (now - stat_t0), (unsigned long)(decode_ms / (frames ? frames : 1)));
            ui_info(info);
            frames = 0;
            decode_ms = 0;
            stat_t0 = now;
        }
    }
    f.close();
    heap_caps_free(buf);
}

/* ---------- task ---------- */

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
        case Cmd::TogglePause:
        case Cmd::Stop:
            break;
        }
    }
}

static bool media_begin(void) {
    if (s_task) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        s_frame[i] = (uint16_t *)psram_alloc((size_t)k_scr_w * k_scr_h * 2);
    }
    /* JPEGDEC in internal RAM: its tables are hit constantly while decoding (faster than PSRAM).
     * PNG (large, with the 4096 px line buffer) goes to PSRAM. */
    void *jm = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!jm) {
        jm = psram_alloc(sizeof(JPEGDEC));
    }
    void *pm = psram_alloc(sizeof(PNG));
    if (!s_frame[0] || !s_frame[1] || !jm || !pm) {
        serial_log("[media] out of memory");
        return false;
    }
    s_jpeg = new (jm) JPEGDEC();
    s_png = new (pm) PNG();
    s_queue = xQueueCreate(4, sizeof(MediaCmd));
    /* Core 1, above loop()/weather (1): decoding should win, the UI lives on core 0 anyway. */
    xTaskCreatePinnedToCore(media_task, "media", 12288, nullptr, 3, &s_task, 1);
    serial_log("[media] ready (JPEGDEC %u B, PNG %u B)", (unsigned)sizeof(JPEGDEC), (unsigned)sizeof(PNG));
    return true;
}

static void send(Cmd cmd, const char *path) {
    MediaCmd c;
    c.cmd = cmd;
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
    apply(s_video_mode ? s_play_btn : nullptr);
    if (show) {
        lv_timer_reset(s_hide_timer);
        lv_timer_resume(s_hide_timer);
    }
}

static void hide_timer_cb(lv_timer_t *t) {
    lv_timer_pause(t);
    set_overlay_visible(false);
}

static void show_image_at(size_t i) {
    s_image_i = i;
    lv_label_set_text_fmt(s_title, "%s  (%u/%u)", base_name(s_images[i].c_str()), (unsigned)(i + 1),
                          (unsigned)s_images.size());
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

static void back_cb(lv_event_t *e) {
    LV_UNUSED(e);
    send(Cmd::Stop, nullptr);
    if (s_return_scr) {
        lv_scr_load(s_return_scr);
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

static void screen_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
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
    s_play_btn = round_btn(s_scr, LV_SYMBOL_PAUSE, play_cb, &s_play_lbl);
    lv_obj_align(s_play_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_hide_timer = lv_timer_create(hide_timer_cb, k_overlay_hide_ms, nullptr);
    lv_timer_pause(s_hide_timer);
}

static void enter(bool video) {
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
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
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
    lv_label_set_text(s_title, base_name(path.c_str()));
    lv_label_set_text(s_info, "");
    lv_label_set_text(s_play_lbl, LV_SYMBOL_PAUSE);
    send(Cmd::PlayVideo, path.c_str());
}
