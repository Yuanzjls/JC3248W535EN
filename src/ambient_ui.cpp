#include "ambient_ui.h"

#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "airthings.h"
#include "app_settings.h"
#include "esp_bsp.h"
#include "home_ui.h"
#include "lv_port.h"
#include "platform_time.h"
#include "sd_card.h"
#include "serial_log.h"
#include "ui_font.h"
#include "weather.h"
#include "weather_ui.h"
#include "wifi_net.h"

/*
 * How it draws: the clip's frames are decoded (esp_new_jpeg, SIMD) into a full-screen buffer,
 * the text layer is blended in, and the result goes straight to the panel - no LVGL compose per
 * frame. The text layer is built from ordinary LVGL widgets on a hidden screen and captured
 * once with lv_snapshot (RGB565 + alpha) when the minute or the data changes; each frame then
 * only blends the pixels that have text or a panel on them (precomputed runs).
 *
 * Clips are 480x320, 24 fps, ~5 s loops; every other frame is shown (12 fps, same speed) to
 * keep the CPU load low for a screen that may run for hours.
 */

static const int k_w = 480;
static const int k_h = 320;
static const uint32_t k_frame_ms = 83; /* 12 fps */
static const int k_frame_step = 2;     /* of the clips' 24 fps */
static const char *k_clip_dir = "/weather";

/* Task <-> UI state */
enum class State { Idle, Running, Stopping };
static volatile State s_state = State::Idle;
static TaskHandle_t s_task = nullptr;

/* LVGL objects (created / deleted under the display lock) */
static lv_obj_t *s_scr = nullptr;    /* visible screen: an lv_img showing the last frame */
static lv_obj_t *s_img = nullptr;
static lv_img_dsc_t s_dsc;
static lv_obj_t *s_ov_scr = nullptr; /* hidden screen holding the text layer widgets */
static lv_obj_t *s_time = nullptr, *s_date = nullptr;
static lv_obj_t *s_temp = nullptr, *s_cond = nullptr, *s_loc = nullptr, *s_extra = nullptr;
static lv_obj_t *s_air_panel = nullptr, *s_radon = nullptr, *s_radon_lvl = nullptr;

/* Buffers (PSRAM) */
static uint16_t *s_fb[2] = {nullptr, nullptr};

/* ---------- what to show ---------- */

struct Info {
    bool wifi;
    bool wvalid;
    bool fahrenheit;
    bool is_day;
    int code;
    int humidity;
    float temp;
    float feels;
    float wind;
    char location[48];
    bool air;
    int radon; /* -1: no value yet */
    char time[16];
    char date[32];
};

/** Gather everything shown (caller holds the display lock: weather/radon data live in the UI). */
static void read_info(Info *in) {
    memset(in, 0, sizeof(*in));
    in->wifi = wifi_net_connected();
    const WeatherData &wd = weather_ui_data();
    in->wvalid = wd.valid;
    if (wd.valid) {
        in->fahrenheit = wd.fahrenheit;
        in->is_day = wd.is_day;
        in->code = wd.code;
        in->humidity = wd.humidity;
        in->temp = wd.temp;
        in->feels = wd.feels_like;
        in->wind = wd.wind;
        strlcpy(in->location, wd.location, sizeof(in->location));
    }
    in->air = airthings_configured();
    const AirData &ad = airthings_data();
    in->radon = (in->air && ad.valid) ? ad.radon_st : -1;
    const time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm ti;
        localtime_r(&now, &ti);
        settings_format_hm(in->time, sizeof(in->time), &ti, true);
        strftime(in->date, sizeof(in->date), "%A, %B %d", &ti);
    } else {
        strlcpy(in->time, "--:--", sizeof(in->time));
    }
}

/** Clip for the conditions (WMO weather code + day/night). See README of the clips: /weather/. */
static const char *pick_clip(const Info &in) {
    if (!in.wifi || !in.wvalid) {
        return "nowifi";
    }
    const int c = in.code;
    const bool d = in.is_day;
    const float temp_c = in.fahrenheit ? (in.temp - 32) * 5 / 9 : in.temp;
    const float wind_kmh = in.fahrenheit ? in.wind * 1.609f : in.wind;
    if (c >= 95) {
        return d ? "thunderstorm_day" : "thunderstorm_night";
    }
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) {
        return d ? "snow_day" : "snow_night";
    }
    if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) {
        return d ? "rain" : "rain_night";
    }
    if (c == 45 || c == 48) {
        return d ? "fog_day" : "fog_night";
    }
    if (wind_kmh >= 40) {
        return d ? "windy_day" : "windy_night";
    }
    if (c == 3) {
        return d ? "cloudy_day" : "cloudy_night";
    }
    if (c == 1 || c == 2) {
        return d ? "partly_sunny" : (c == 1 ? "clear" : "cloudy_night");
    }
    if (d && temp_c >= 30) {
        return "hot";
    }
    return d ? "sunny" : "clear";
}

/* ---------- text layer ---------- */

static lv_obj_t *make_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_40, 0); /* readable on bright skies, sky still shows */
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_ver(p, 10, 0);
    lv_obj_set_style_pad_row(p, 2, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, "");
    return l;
}

/** Hidden screen with the widgets of the text layer (LVGL context). */
static void build_overlay_widgets(void) {
    s_ov_scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_ov_scr);
    lv_obj_set_size(s_ov_scr, k_w, k_h);
    lv_obj_set_style_bg_opa(s_ov_scr, LV_OPA_TRANSP, 0);

    lv_obj_t *tp = make_panel(s_ov_scr);
    lv_obj_align(tp, LV_ALIGN_TOP_LEFT, 14, 14);
    s_time = make_label(tp, &lv_font_montserrat_48);
    s_date = make_label(tp, &lv_font_montserrat_18);

    lv_obj_t *wp = make_panel(s_ov_scr);
    lv_obj_align(wp, LV_ALIGN_BOTTOM_LEFT, 14, -14);
    s_temp = make_label(wp, &lv_font_montserrat_48);
    s_cond = make_label(wp, &lv_font_montserrat_22);
    s_loc = make_label(wp, &lv_font_montserrat_16);
    s_extra = make_label(wp, &lv_font_montserrat_16);

    s_air_panel = make_panel(s_ov_scr);
    lv_obj_align(s_air_panel, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_set_style_text_align(s_air_panel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *cap = make_label(s_air_panel, &lv_font_montserrat_14);
    lv_label_set_text(cap, "Radon, 24 h");
    s_radon = make_label(s_air_panel, &lv_font_montserrat_28);
    s_radon_lvl = make_label(s_air_panel, &lv_font_montserrat_16);
}

static bool has_non_ascii(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

static void fill_overlay_widgets(const Info &in) {
    lv_label_set_text(s_time, in.time);
    lv_label_set_text(s_date, in.date[0] ? in.date : "Waiting for time sync");
    const char *deg = in.fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
    if (in.wvalid) {
        /* snprintf, not lv_label_set_text_fmt: LVGL's printf is built without float support. */
        char buf[48];
        snprintf(buf, sizeof(buf), "%.0f%s", in.temp, deg);
        lv_label_set_text(s_temp, buf);
        lv_label_set_text(s_cond, weather_code_text(in.code));
        lv_obj_set_style_text_font(s_loc, has_non_ascii(in.location) ? ui_font_get() : &lv_font_montserrat_16, 0);
        lv_label_set_text(s_loc, in.location);
        snprintf(buf, sizeof(buf), "Feels %.0f%s   Humidity %d%%", in.feels, deg, in.humidity);
        lv_label_set_text(s_extra, buf);
    } else {
        lv_label_set_text(s_temp, "--");
        lv_label_set_text(s_cond, in.wifi ? "Loading weather..." : "No Wi-Fi");
        lv_label_set_text(s_loc, "");
        lv_label_set_text(s_extra, "");
    }
    if (in.air) {
        lv_obj_clear_flag(s_air_panel, LV_OBJ_FLAG_HIDDEN);
        if (in.radon >= 0) {
            /* Same levels as the Air app: < 100 good, < 150 fair, else poor (Bq/m3). */
            const lv_palette_t p = in.radon < 100 ? LV_PALETTE_GREEN : in.radon < 150 ? LV_PALETTE_AMBER : LV_PALETTE_RED;
            lv_label_set_text_fmt(s_radon, "%d Bq/m3", in.radon);
            lv_label_set_text(s_radon_lvl, in.radon < 100 ? "Good" : in.radon < 150 ? "Fair" : "Poor");
            lv_obj_set_style_text_color(s_radon_lvl, lv_palette_lighten(p, 2), 0);
        } else {
            lv_label_set_text(s_radon, "--");
            lv_label_set_text(s_radon_lvl, "");
        }
    } else {
        lv_obj_add_flag(s_air_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Captured text layer: RGB565 (big-endian, as the frames) + alpha per pixel, and the runs of
 * pixels with alpha > 0 so blending skips the transparent ~70 % of the screen. */
struct Run {
    uint32_t off; /* pixel index */
    uint32_t len;
};
static lv_img_dsc_t *s_ov = nullptr;
static Run *s_runs = nullptr;
static size_t s_run_n = 0;

/** Re-capture the text layer (caller holds the display lock). */
static void capture_overlay(const Info &in) {
    fill_overlay_widgets(in);
    lv_obj_update_layout(s_ov_scr);
    lv_img_dsc_t *snap = lv_snapshot_take(s_ov_scr, LV_IMG_CF_TRUE_COLOR_ALPHA);
    if (!snap || snap->header.w != k_w || snap->header.h != k_h) {
        if (snap) {
            lv_snapshot_free(snap);
        }
        serial_log("[ambient] text layer capture failed");
        return;
    }
    /* Runs: two passes (count, then fill) so the array is exactly sized, in PSRAM. */
    const uint8_t *px = snap->data;
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        Run *out = nullptr;
        if (pass == 1) {
            out = (Run *)heap_caps_malloc((n ? n : 1) * sizeof(Run), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!out) {
                lv_snapshot_free(snap);
                return;
            }
        }
        size_t k = 0;
        for (int y = 0; y < k_h; y++) {
            int x = 0;
            while (x < k_w) {
                while (x < k_w && px[((size_t)y * k_w + x) * 3 + 2] == 0) {
                    x++;
                }
                const int start = x;
                while (x < k_w && px[((size_t)y * k_w + x) * 3 + 2] != 0) {
                    x++;
                }
                if (x > start) {
                    if (out) {
                        out[k] = {(uint32_t)(y * k_w + start), (uint32_t)(x - start)};
                    }
                    k++;
                }
            }
        }
        if (pass == 0) {
            n = k;
        } else {
            if (s_ov) {
                lv_snapshot_free(s_ov);
            }
            heap_caps_free(s_runs);
            s_ov = snap;
            s_runs = out;
            s_run_n = n;
        }
    }
}

/** Blend the text layer into a decoded frame (both big-endian RGB565). */
static void blend_overlay(uint16_t *frame) {
    if (!s_ov) {
        return;
    }
    uint8_t *fb = (uint8_t *)frame;
    const uint8_t *ov = s_ov->data;
    for (size_t r = 0; r < s_run_n; r++) {
        const Run run = s_runs[r];
        uint8_t *d = fb + (size_t)run.off * 2;
        const uint8_t *s = ov + (size_t)run.off * 3;
        for (uint32_t i = 0; i < run.len; i++, d += 2, s += 3) {
            const uint32_t a = s[2];
            if (a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                continue;
            }
            const uint32_t sc = ((uint32_t)s[0] << 8) | s[1];
            const uint32_t dc = ((uint32_t)d[0] << 8) | d[1];
            const uint32_t ia = 255 - a;
            const uint32_t r5 = (((sc >> 11) & 31) * a + ((dc >> 11) & 31) * ia) / 255;
            const uint32_t g6 = (((sc >> 5) & 63) * a + ((dc >> 5) & 63) * ia) / 255;
            const uint32_t b5 = ((sc & 31) * a + (dc & 31) * ia) / 255;
            const uint32_t c = (r5 << 11) | (g6 << 5) | b5;
            d[0] = (uint8_t)(c >> 8);
            d[1] = (uint8_t)c;
        }
    }
}

/* ---------- clip ---------- */

static uint8_t *s_clip = nullptr; /* whole file in PSRAM */
static size_t s_clip_len = 0;
static uint32_t *s_frames = nullptr; /* frame start offsets */
static size_t s_frame_n = 0;
static char s_clip_name[24] = "";

static void free_clip(void) {
    heap_caps_free(s_clip);
    heap_caps_free(s_frames);
    s_clip = nullptr;
    s_frames = nullptr;
    s_clip_len = s_frame_n = 0;
    s_clip_name[0] = '\0';
}

/** Read /weather/<name>.mjpeg into PSRAM and index its frames. False (no clip) if missing. */
static bool load_clip(const char *name) {
    free_clip();
    strlcpy(s_clip_name, name, sizeof(s_clip_name)); /* remembered even if missing: no retry loop */
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.mjpeg", k_clip_dir, name);
    const uint32_t t0 = now_ms();
    const int fd = sd_open_read(path);
    if (fd < 0) {
        serial_log("[ambient] %s not found - plain background", path);
        return false;
    }
    const size_t len = sd_file_size(fd);
    s_clip = (uint8_t *)heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t got = s_clip ? sd_read(fd, s_clip, len) : 0;
    close(fd);
    if (!s_clip || got != len || len < 4) {
        serial_log("[ambient] cannot read %s", path);
        free_clip();
        strlcpy(s_clip_name, name, sizeof(s_clip_name));
        return false;
    }
    s_clip_len = len;
    size_t n = 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (s_clip[i] == 0xFF && s_clip[i + 1] == 0xD8 && s_clip[i + 2] == 0xFF) {
            n++;
        }
    }
    s_frames = (uint32_t *)heap_caps_malloc((n + 1) * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frames || n == 0) {
        serial_log("[ambient] %s has no JPEG frames", path);
        free_clip();
        strlcpy(s_clip_name, name, sizeof(s_clip_name));
        return false;
    }
    size_t k = 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (s_clip[i] == 0xFF && s_clip[i + 1] == 0xD8 && s_clip[i + 2] == 0xFF) {
            s_frames[k++] = (uint32_t)i;
        }
    }
    s_frames[k] = (uint32_t)len;
    s_frame_n = k;
    serial_log("[ambient] clip %s: %u frames, %u KB, read in %lu ms", name, (unsigned)k, (unsigned)(len / 1024),
               (unsigned long)(now_ms() - t0));
    return true;
}

/* ---------- task ---------- */

static void cleanup(jpeg_dec_handle_t dec) {
    if (dec) {
        jpeg_dec_close(dec);
    }
    free_clip();
    /* Home is already on screen (ambient_ui_close loaded it): our screens are not drawn any more. */
    bsp_display_lock(0);
    if (s_ov) {
        lv_snapshot_free(s_ov);
        s_ov = nullptr;
    }
    heap_caps_free(s_runs);
    s_runs = nullptr;
    s_run_n = 0;
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = s_img = nullptr;
    }
    if (s_ov_scr) {
        lv_obj_del(s_ov_scr);
        s_ov_scr = nullptr;
    }
    for (auto &b : s_fb) {
        heap_caps_free(b);
        b = nullptr;
    }
    bsp_display_unlock();
}

static void play(void) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        dec = nullptr;
    }
    Info shown;
    memset(&shown, 0xFF, sizeof(shown)); /* differs from anything real: first pass captures */
    size_t frame = 0;
    int back = 0;
    uint32_t next_due = now_ms(), next_check = 0;
    uint32_t frames = 0, decode_ms = 0, log_t0 = now_ms();

    while (s_state == State::Running) {
        /* Once a second: data / minute changed -> new text layer; conditions changed -> new clip. */
        if ((int32_t)(now_ms() - next_check) >= 0) {
            next_check = now_ms() + 1000;
            Info in;
            bsp_display_lock(0);
            read_info(&in);
            if (memcmp(&in, &shown, sizeof(in)) != 0) {
                capture_overlay(in);
                shown = in;
            }
            bsp_display_unlock();
            const char *want = pick_clip(in);
            if (strcmp(want, s_clip_name) != 0) {
                load_clip(want);
                frame = 0;
            }
        }

        /* Next frame: decode (or plain dark background without a clip), blend, show. */
        uint16_t *fb = s_fb[back];
        const uint32_t t0 = now_ms();
        bool ok = false;
        if (dec && s_frame_n) {
            jpeg_dec_io_t io = {};
            io.inbuf = s_clip + s_frames[frame];
            io.inbuf_len = (int)(s_frames[frame + 1] - s_frames[frame]);
            jpeg_dec_header_info_t hi;
            if (jpeg_dec_parse_header(dec, &io, &hi) == JPEG_ERR_OK && hi.width == k_w && hi.height == k_h) {
                io.outbuf = (uint8_t *)fb;
                ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
            }
            frame = (frame + k_frame_step) % s_frame_n;
        }
        if (!ok) {
            const uint16_t bg = 0x2108; /* dark grey-blue, byte-swapped RGB565 */
            for (int i = 0; i < k_w * k_h; i++) {
                fb[i] = bg;
            }
        }
        blend_overlay(fb);
        decode_ms += now_ms() - t0;

        const int32_t wait = (int32_t)(next_due - now_ms());
        if (wait > 0) {
            sleep_ms((uint32_t)wait);
        }
        next_due = now_ms() + k_frame_ms;
        bsp_display_lock(0);
        if (s_state == State::Running && s_scr && lv_scr_act() == s_scr) {
            s_dsc.data = (const uint8_t *)fb; /* what LVGL shows if it ever redraws this screen */
            lvgl_port_flush_frame(lv_disp_get_default(), (const lv_color_t *)fb);
        }
        bsp_display_unlock();
        back ^= 1;
        frames++;

        if (now_ms() - log_t0 >= 60000) {
            serial_log("[ambient] %s: %.1f fps, decode+blend %lu ms", s_clip_name, frames * 1000.0f / (now_ms() - log_t0),
                       (unsigned long)(decode_ms / (frames ? frames : 1)));
            frames = decode_ms = 0;
            log_t0 = now_ms();
        }
    }
    cleanup(dec);
}

static void ambient_task(void *arg) {
    LV_UNUSED(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_state == State::Running) {
            play();
        }
        s_state = State::Idle;
        serial_log("[ambient] closed");
    }
}

/* ---------- open / close (LVGL context) ---------- */

static void close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_state != State::Running) {
        return;
    }
    s_state = State::Stopping; /* the task frees everything once it notices */
    lv_indev_wait_release(lv_indev_get_act());
    home_ui_show();
}

bool ambient_ui_open(void) {
    if (s_state != State::Idle) {
        return false;
    }
    if (!s_task) {
        /*
         * Core 0, like every task that sends frames (LVGL task, video show task): the display's SPI
         * interrupt is on core 0, and a sender on core 1 eventually hangs in spi_device_acquire_bus
         * holding the display lock (full UI freeze - seen after ~80 min on core 1). Core 0 is
         * nearly idle while this screen shows; priority 2 stays below LVGL (4) and Wi-Fi.
         * PSRAM stack: never touches flash.
         */
        xTaskCreatePinnedToCoreWithCaps(ambient_task, "ambient", 6144, nullptr, 2, &s_task, 0,
                                        MALLOC_CAP_SPIRAM);
        if (!s_task) {
            return false;
        }
    }
    const size_t fb_bytes = (size_t)k_w * k_h * 2;
    for (auto &b : s_fb) {
        /* 16-byte aligned: esp_new_jpeg writes its output with SIMD stores */
        b = (uint16_t *)heap_caps_aligned_alloc(16, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b) {
            for (auto &f : s_fb) {
                heap_caps_free(f);
                f = nullptr;
            }
            serial_log("[ambient] out of memory");
            return false;
        }
        memset(b, 0, fb_bytes);
    }

    s_scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, close_cb, LV_EVENT_PRESSED, nullptr);
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.w = k_w;
    s_dsc.header.h = k_h;
    s_dsc.data_size = (uint32_t)fb_bytes;
    s_dsc.data = (const uint8_t *)s_fb[0];
    s_img = lv_img_create(s_scr);
    lv_img_set_src(s_img, &s_dsc);
    lv_obj_center(s_img);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE); /* taps go to the screen */
    build_overlay_widgets();

    lv_scr_load(s_scr);
    s_state = State::Running;
    xTaskNotifyGive(s_task);
    serial_log("[ambient] opened");
    return true;
}
