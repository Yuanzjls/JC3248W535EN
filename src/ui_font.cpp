#include "ui_font.h"

#include <dirent.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sd_card.h"
#include "serial_log.h"
#include "platform_time.h"

/** Must match include/lv_conf.h LV_FS_POSIX_PATH and the SD mount point (sd_card.cpp) */
static const char *k_sd_mount = "/sd";

/*
 * Card paths ("/fonts/...") for our own reads (sd_open_read adds the /sd mount point); LVGL
 * paths ("S:/fonts/...") for lv_fs, which maps "S:" to LV_FS_POSIX_PATH.
 */
static const char *const k_try_card[] = {"/fonts/UI_FONT_16.bin", "/UI_FONT_16.bin"};
static const char *const k_try_lvgl[] = {"S:/fonts/UI_FONT_16.bin", "S:/UI_FONT_16.bin"};

static const size_t k_try_n = sizeof(k_try_card) / sizeof(k_try_card[0]);

static lv_font_t *s_loaded_font = nullptr;

/** Resolved pair; nullptr until pick_font_file() succeeds */
static const char *s_path_card = nullptr;
static const char *s_path_lvgl = nullptr;

static void sd_list_root(void) {
    DIR *root = opendir(k_sd_mount);
    if (!root) {
        serial_log("[ui_font] diag: cannot open SD root (mount %s) — SD card not mounted?", k_sd_mount);
        return;
    }
    serial_log("[ui_font] diag: listing card root %s/ (max 24 entries)", k_sd_mount);
    unsigned n = 0;
    struct dirent *de;
    while (n < 24 && (de = readdir(root)) != nullptr) {
        struct stat st = {};
        const std::string full = std::string(k_sd_mount) + "/" + de->d_name;
        (void)stat(full.c_str(), &st);
        serial_log("[ui_font]   %s%s %lu", de->d_name, S_ISDIR(st.st_mode) ? "/" : "", (unsigned long)st.st_size);
        n++;
    }
    closedir(root);
}

/** Pick first path where the file exists; log help if none */
static bool pick_font_file(void) {
    s_path_card = nullptr;
    s_path_lvgl = nullptr;
    for (size_t i = 0; i < k_try_n; i++) {
        struct stat st;
        if (stat(sd_path(k_try_card[i]).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            s_path_card = k_try_card[i];
            s_path_lvgl = k_try_lvgl[i];
            serial_log("[ui_font] font file: %s", s_path_card);
            return true;
        }
    }
    serial_log("[ui_font] no UI_FONT_16.bin — add fonts/UI_FONT_16.bin or UI_FONT_16.bin at card root");
    sd_list_root();
    return false;
}

static void font_load_diagnose(void) {
    if (!pick_font_file()) {
        return;
    }
    const int fd = sd_open_read(s_path_card);
    if (fd < 0) {
        serial_log("[ui_font] diag: reopen failed %s", s_path_card);
        return;
    }
    const size_t sz = sd_file_size(fd);
    uint8_t hdr[16];
    const ssize_t r = read(fd, hdr, sizeof(hdr));
    const size_t n = r > 0 ? (size_t)r : 0;
    close(fd);
    serial_log("[ui_font] diag: file bytes=%lu read=%u", (unsigned long)sz, (unsigned)n);
    if (n >= 8) {
        serial_log("[ui_font] diag: hdr[4..7]=%02x %02x %02x %02x (expect head=68 65 61 64)", hdr[4], hdr[5],
                   hdr[6], hdr[7]);
        const bool ok = (hdr[4] == 'h' && hdr[5] == 'e' && hdr[6] == 'a' && hdr[7] == 'd');
        if (!ok) {
            serial_log("[ui_font] diag: not LVGL v8 bin — use https://lvgl.io/tools bin v8");
        }
    }
    lv_fs_file_t lvf;
    const lv_fs_res_t res = lv_fs_open(&lvf, s_path_lvgl, LV_FS_MODE_RD);
    serial_log("[ui_font] diag: lv_fs_open(%s)=%d (0=OK)", s_path_lvgl, (int)res);
    if (res == LV_FS_RES_OK) {
        lv_fs_close(&lvf);
    }
}

/*
 * In-memory LVGL drive 'M': lv_font_load() does thousands of small reads/seeks, which is very slow
 * straight from SD (~16 s for a 2.3 MB font). We read the file into PSRAM in big sequential chunks
 * and let LVGL parse it from RAM instead.
 */
static const char k_mem_letter = 'M';
static const uint8_t *s_mem_buf = nullptr;
static size_t s_mem_len = 0;
static size_t s_mem_pos = 0;
static bool s_mem_drv_registered = false;

static void *mem_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    LV_UNUSED(path);
    if (!s_mem_buf || mode != LV_FS_MODE_RD) {
        return nullptr;
    }
    s_mem_pos = 0;
    return (void *)&s_mem_pos;
}

static lv_fs_res_t mem_close_cb(lv_fs_drv_t *drv, void *file_p) {
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t mem_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    const size_t left = s_mem_len - s_mem_pos;
    const size_t n = btr < left ? btr : left;
    memcpy(buf, s_mem_buf + s_mem_pos, n);
    s_mem_pos += n;
    *br = (uint32_t)n;
    return LV_FS_RES_OK;
}

static lv_fs_res_t mem_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    size_t base = 0;
    if (whence == LV_FS_SEEK_CUR) {
        base = s_mem_pos;
    } else if (whence == LV_FS_SEEK_END) {
        base = s_mem_len;
    }
    const size_t target = base + pos;
    if (target > s_mem_len) {
        return LV_FS_RES_INV_PARAM;
    }
    s_mem_pos = target;
    return LV_FS_RES_OK;
}

static lv_fs_res_t mem_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    LV_UNUSED(drv);
    LV_UNUSED(file_p);
    *pos_p = (uint32_t)s_mem_pos;
    return LV_FS_RES_OK;
}

static void mem_drv_register(void) {
    if (s_mem_drv_registered) {
        return;
    }
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = k_mem_letter;
    drv.open_cb = mem_open_cb;
    drv.close_cb = mem_close_cb;
    drv.read_cb = mem_read_cb;
    drv.seek_cb = mem_seek_cb;
    drv.tell_cb = mem_tell_cb;
    lv_fs_drv_register(&drv);
    s_mem_drv_registered = true;
}

/** Read the whole font file into PSRAM and parse it from there. Returns nullptr on any failure. */
static lv_font_t *load_font_via_psram(const char *card_path) {
    const uint32_t t0 = now_ms();
    const int fd = sd_open_read(card_path);
    if (fd < 0) {
        return nullptr;
    }
    const size_t len = sd_file_size(fd);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        serial_log("[ui_font] PSRAM alloc of %lu bytes failed", (unsigned long)len);
        close(fd);
        return nullptr;
    }
    size_t got = 0;
    while (got < len) {
        const size_t chunk = (len - got) < 32768 ? (len - got) : 32768;
        const size_t n = sd_read(fd, buf + got, chunk); /* PSRAM: via DMA bounce buffer */
        if (n == 0) {
            break;
        }
        got += n;
    }
    close(fd);
    const uint32_t t_read = now_ms() - t0;
    if (got != len) {
        serial_log("[ui_font] short read %lu/%lu bytes", (unsigned long)got, (unsigned long)len);
        heap_caps_free(buf);
        return nullptr;
    }

    mem_drv_register();
    s_mem_buf = buf;
    s_mem_len = len;
    const uint32_t t1 = now_ms();
    lv_font_t *font = lv_font_load("M:font");
    serial_log("[ui_font] PSRAM path: SD read %lu ms (%lu KB/s), parse %lu ms", (unsigned long)t_read,
               (unsigned long)(t_read ? len / t_read : 0), (unsigned long)(now_ms() - t1));

    /* lv_font_load copies everything it needs; the raw file buffer can go. */
    s_mem_buf = nullptr;
    s_mem_len = 0;
    heap_caps_free(buf);
    return font;
}

bool ui_font_load_from_sd(void) {
    if (s_loaded_font) {
        serial_log("[ui_font] font: already loaded (cached)");
        return true;
    }
    serial_log("[ui_font] font: loading from SD ...");
    font_load_diagnose();
    const uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    serial_log("[ui_font] diag: free heap=%lu", (unsigned long)free_heap);

    if (!s_path_lvgl) {
        serial_log("[ui_font] font: LOAD FAILED (file not found) — SimSun fallback");
        return false;
    }

    s_loaded_font = load_font_via_psram(s_path_card);
    if (!s_loaded_font) {
        serial_log("[ui_font] PSRAM path failed — falling back to slow direct SD load");
        s_loaded_font = lv_font_load(s_path_lvgl);
    }
    if (!s_loaded_font) {
        serial_log("[ui_font] font: LOAD FAILED (parse/OOM) — SimSun fallback");
        return false;
    }
    s_loaded_font->fallback = &lv_font_montserrat_14;
    serial_log("[ui_font] font: LOAD OK (line_height=%d)", (int)s_loaded_font->line_height);
    return true;
}

const lv_font_t *ui_font_get(void) {
    if (s_loaded_font) {
        return s_loaded_font;
    }
    return &lv_font_simsun_16_cjk;
}
