#include "cover_art.h"

#include <lvgl.h> /* before PNGdec: see the #undef below */

#include <JPEGDEC.h>
#include <PNGdec.h>
/* PNGdec's bundled zlib does `#define local static` in a public header (see media_ui.cpp). */
#undef local

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <new>
#include <string.h>
#include <unistd.h>

#include "platform_time.h"
#include "sd_card.h"
#include "serial_log.h"

/*
 * One worker task (core 1, below the audio task) reads the picture bytes into PSRAM, decodes them
 * (JPEGDEC at the smallest 1/2^n scale still >= the target, or PNGdec at full size) and
 * area-averages the result down to the target. Requests go through a one-slot queue (a newer
 * track replaces an older pending one); the result sits in a one-slot mailbox until taken.
 * Pixels are big-endian RGB565 because lv_conf.h has LV_COLOR_16_SWAP=1.
 */

static const uint32_t k_max_file = 4 * 1024 * 1024; /* bigger embedded pictures are ignored */
static const size_t k_max_pixels = 2048 * 2048;     /* PNG decoded at full size: cap the buffer */

struct Request {
    char path[256];
    uint32_t off, len;
    int size;
    uint32_t id;
};

static QueueHandle_t s_req_q = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready = false; /* mailbox, under s_mux */
static uint32_t s_res_id = 0;
static uint16_t *s_res_buf = nullptr;
static int s_res_w = 0, s_res_h = 0;

static JPEGDEC *s_jpeg = nullptr;
static PNG *s_png = nullptr;

static void *psram_alloc(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

/** Area-average (or, when enlarging, nearest) resample of big-endian RGB565 src (sw x sh) into dst (dw x dh). */
static void resample(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        const int y0 = y * sh / dh;
        int y1 = (y + 1) * sh / dh;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (int x = 0; x < dw; x++) {
            const int x0 = x * sw / dw;
            int x1 = (x + 1) * sw / dw;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint16_t *row = src + (size_t)sy * sw;
                for (int sx = x0; sx < x1; sx++) {
                    const uint16_t p = swap16(row[sx]);
                    r += p >> 11;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                }
            }
            const uint32_t n = (uint32_t)(y1 - y0) * (x1 - x0);
            dst[(size_t)y * dw + x] = swap16((uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n)));
        }
    }
}

/* ---------- JPEG ---------- */

struct JpegTarget {
    uint16_t *buf;
    int w, h;
};

static int jpeg_draw_cb(JPEGDRAW *d) {
    const JpegTarget *t = (const JpegTarget *)d->pUser;
    const int w = (d->x + d->iWidthUsed > t->w) ? t->w - d->x : d->iWidthUsed;
    for (int r = 0; r < d->iHeight && d->y + r < t->h; r++) {
        if (w > 0) {
            memcpy(t->buf + (size_t)(d->y + r) * t->w + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)w * 2);
        }
    }
    return 1;
}

/** Decode to a new buffer at roughly >= size px; *w x *h is its size. */
static uint16_t *decode_jpeg(uint8_t *data, size_t len, int size, int *w, int *h) {
    if (!s_jpeg->openRAM(data, (int)len, jpeg_draw_cb)) {
        return nullptr;
    }
    const int iw = s_jpeg->getWidth(), ih = s_jpeg->getHeight();
    const bool progressive = s_jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
    /* Progressive JPEGs are always decoded at 1/8 by the library (first scan only). */
    int s = 8;
    while (!progressive && s > 1 && (iw / s < size || ih / s < size)) {
        s /= 2;
    }
    const int option = progressive ? 0 : s == 2 ? JPEG_SCALE_HALF : s == 4 ? JPEG_SCALE_QUARTER : s == 8 ? JPEG_SCALE_EIGHTH : 0;
    *w = (iw + s - 1) / s;
    *h = (ih + s - 1) / s;
    uint16_t *buf = (uint16_t *)psram_alloc((size_t)*w * *h * 2);
    if (!buf) {
        s_jpeg->close();
        return nullptr;
    }
    memset(buf, 0, (size_t)*w * *h * 2); /* scaled output can be 1 px short of the rounded-up size */
    JpegTarget t = {buf, *w, *h};
    s_jpeg->setPixelType(RGB565_BIG_ENDIAN);
    s_jpeg->setUserPointer(&t);
    const int ok = s_jpeg->decode(0, 0, option);
    s_jpeg->close();
    if (!ok) {
        heap_caps_free(buf);
        return nullptr;
    }
    return buf;
}

/* ---------- PNG ---------- */

struct PngTarget {
    uint16_t *buf;
    int w;
};

static int png_draw_cb(PNGDRAW *d) {
    const PngTarget *t = (const PngTarget *)d->pUser;
    s_png->getLineAsRGB565(d, t->buf + (size_t)d->y * t->w, PNG_RGB565_BIG_ENDIAN, 0xFFFFFF);
    return 1;
}

static uint16_t *decode_png(uint8_t *data, size_t len, int *w, int *h) {
    if (s_png->openRAM(data, (int)len, png_draw_cb) != PNG_SUCCESS) {
        return nullptr;
    }
    *w = s_png->getWidth();
    *h = s_png->getHeight();
    if ((*w * 4 + 1) * 2 > PNG_MAX_BUFFERED_PIXELS || (size_t)*w * *h > k_max_pixels) {
        s_png->close();
        return nullptr;
    }
    uint16_t *buf = (uint16_t *)psram_alloc((size_t)*w * *h * 2);
    if (!buf) {
        s_png->close();
        return nullptr;
    }
    PngTarget t = {buf, *w};
    const int rc = s_png->decode(&t, 0);
    s_png->close();
    if (rc != PNG_SUCCESS) {
        heap_caps_free(buf);
        return nullptr;
    }
    return buf;
}

/* ---------- worker ---------- */

/** Read and decode one request; nullptr on any failure. */
static uint16_t *decode_request(const Request &r, int *out_w, int *out_h) {
    if (r.len < 8 || r.len > k_max_file) {
        return nullptr;
    }
    const int fd = sd_open_read(r.path);
    if (fd < 0) {
        return nullptr;
    }
    uint8_t *data = (uint8_t *)psram_alloc(r.len);
    const bool read_ok = data && lseek(fd, r.off, SEEK_SET) == (off_t)r.off && sd_read(fd, data, r.len) == r.len;
    close(fd);
    if (!read_ok) {
        heap_caps_free(data);
        return nullptr;
    }

    int w = 0, h = 0;
    uint16_t *img = nullptr;
    if (data[0] == 0xFF && data[1] == 0xD8) {
        img = decode_jpeg(data, r.len, r.size, &w, &h);
    } else if (memcmp(data, "\x89PNG", 4) == 0) {
        img = decode_png(data, r.len, &w, &h);
    }
    heap_caps_free(data);
    if (!img || w <= 0 || h <= 0) {
        heap_caps_free(img);
        return nullptr;
    }

    /* Fit inside size x size, keeping the aspect ratio. */
    int dw = r.size, dh = r.size;
    if (w > h) {
        dh = h * r.size / w;
    } else if (h > w) {
        dw = w * r.size / h;
    }
    dw = dw < 1 ? 1 : dw;
    dh = dh < 1 ? 1 : dh;
    uint16_t *out = (uint16_t *)psram_alloc((size_t)dw * dh * 2);
    if (out) {
        resample(img, w, h, out, dw, dh);
        *out_w = dw;
        *out_h = dh;
    }
    heap_caps_free(img);
    return out;
}

static void cover_task(void *arg) {
    (void)arg;
    Request r;
    for (;;) {
        if (xQueueReceive(s_req_q, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t t0 = now_ms();
        int w = 0, h = 0;
        uint16_t *buf = decode_request(r, &w, &h);
        serial_log("[cover] %s: %s in %lu ms", r.path, buf ? "ok" : "not decodable", (unsigned long)(now_ms() - t0));

        uint16_t *old = nullptr;
        portENTER_CRITICAL(&s_mux);
        if (s_ready) {
            old = s_res_buf; /* never taken: superseded */
        }
        s_ready = true;
        s_res_id = r.id;
        s_res_buf = buf;
        s_res_w = w;
        s_res_h = h;
        portEXIT_CRITICAL(&s_mux);
        heap_caps_free(old);
    }
}

static bool ensure_started(void) {
    if (s_req_q) {
        return true;
    }
    /* Both in PSRAM: covers are small, and internal RAM is needed by BLE and the SD driver. */
    void *jm = psram_alloc(sizeof(JPEGDEC));
    void *pm = psram_alloc(sizeof(PNG));
    if (!jm || !pm) {
        heap_caps_free(jm);
        heap_caps_free(pm);
        serial_log("[cover] out of memory");
        return false;
    }
    s_jpeg = new (jm) JPEGDEC();
    s_png = new (pm) PNG();
    s_req_q = xQueueCreate(1, sizeof(Request));
    /* Core 1 below the audio task (5); PSRAM stack: never touch flash (NVS etc.) here. */
    xTaskCreatePinnedToCoreWithCaps(cover_task, "cover", 8192, nullptr, 2, nullptr, 1, MALLOC_CAP_SPIRAM);
    return true;
}

void cover_art_request(const char *path, uint32_t offset, uint32_t len, int size, uint32_t id) {
    if (!ensure_started()) {
        return;
    }
    Request r = {};
    strncpy(r.path, path, sizeof(r.path) - 1);
    r.off = offset;
    r.len = len;
    r.size = size;
    r.id = id;
    xQueueOverwrite(s_req_q, &r);
}

bool cover_art_take(uint32_t *id, uint16_t **buf, int *w, int *h) {
    if (!s_req_q) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    const bool ready = s_ready;
    if (ready) {
        s_ready = false;
        *id = s_res_id;
        *buf = s_res_buf;
        *w = s_res_w;
        *h = s_res_h;
        s_res_buf = nullptr;
    }
    portEXIT_CRITICAL(&s_mux);
    return ready;
}
