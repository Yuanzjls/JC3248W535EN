#include "ui_font.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>
#include "serial_log.h"

/* Same wiring as board (HSPI SD card) */
#ifndef SDCARD_CS
#define SDCARD_CS 10
#define SDCARD_MISO 13
#define SDCARD_MOSI 11
#define SDCARD_SCK 12
#endif

/** Must match include/lv_conf.h LV_FS_POSIX_PATH and Arduino SD mount point */
static const char *k_sd_mount = "/sd";

/*
 * Arduino SD paths are relative to the mount: VFSImpl does mountpoint+fpath, so use
 * "/fonts/..." not "/sd/fonts/..." (the latter becomes /sd/sd/fonts/...).
 * LVGL uses LV_FS_POSIX_PATH + path after "S:" — keep S:/... as-is.
 */
static const char *const k_try_arduino[] = {"/fonts/UI_FONT_16.bin", "/UI_FONT_16.bin"};
static const char *const k_try_lvgl[] = {"S:/fonts/UI_FONT_16.bin", "S:/UI_FONT_16.bin"};

static const size_t k_try_n = sizeof(k_try_arduino) / sizeof(k_try_arduino[0]);

static SPIClass s_sd_spi(HSPI);
static lv_font_t *s_loaded_font = nullptr;

/** Resolved pair; nullptr until pick_font_file() succeeds */
static const char *s_path_arduino = nullptr;
static const char *s_path_lvgl = nullptr;

static void sd_list_root(void) {
    File root = SD.open("/");
    if (!root) {
        serial_log("[ui_font] diag: cannot open SD root (mount %s) — SD.begin failed or card missing?", k_sd_mount);
        return;
    }
    if (!root.isDirectory()) {
        serial_log("[ui_font] diag: SD root is not a directory");
        root.close();
        return;
    }
    serial_log("[ui_font] diag: listing card root %s/ (max 24 entries)", k_sd_mount);
    File f = root.openNextFile();
    unsigned n = 0;
    while (f && n < 24) {
        const char *nm = f.name();
        serial_log("[ui_font]   %s%s %lu", nm, f.isDirectory() ? "/" : "", (unsigned long)f.size());
        f = root.openNextFile();
        n++;
    }
    root.close();
}

/** Pick first path where the file exists; log help if none */
static bool pick_font_file(void) {
    s_path_arduino = nullptr;
    s_path_lvgl = nullptr;
    for (size_t i = 0; i < k_try_n; i++) {
        File file = SD.open(k_try_arduino[i], FILE_READ);
        if (file) {
            file.close();
            s_path_arduino = k_try_arduino[i];
            s_path_lvgl = k_try_lvgl[i];
            serial_log("[ui_font] font file: %s", s_path_arduino);
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
    File f = SD.open(s_path_arduino, FILE_READ);
    if (!f) {
        serial_log("[ui_font] diag: reopen failed %s", s_path_arduino);
        return;
    }
    const size_t sz = f.size();
    uint8_t hdr[16];
    const size_t n = f.read(hdr, sizeof(hdr));
    f.close();
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

bool ui_font_mount_sd(void) {
    serial_log("[ui_font] SD: initializing SPI (HSPI)...");
    s_sd_spi.begin(SDCARD_SCK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS);
    if (!SD.begin(SDCARD_CS, s_sd_spi, 4000000, k_sd_mount, 5, false)) {
        serial_log("[ui_font] SD: mount FAILED (mount point \"%s\")", k_sd_mount);
        return false;
    }
    uint8_t t = SD.cardType();
    const char *type =
        (t == CARD_NONE) ? "none" : (t == CARD_MMC) ? "MMC" : (t == CARD_SD) ? "SDSC" : (t == CARD_SDHC) ? "SDHC" : "unknown";
    const unsigned long mb = (unsigned long)(SD.cardSize() / (1024U * 1024U));
    serial_log("[ui_font] SD: mount OK at \"%s\", type=%s, size=%lu MiB", k_sd_mount, type, mb);
    return true;
}

bool ui_font_load_from_sd(void) {
    if (s_loaded_font) {
        serial_log("[ui_font] font: already loaded (cached)");
        return true;
    }
    serial_log("[ui_font] font: loading from SD ...");
    font_load_diagnose();
    const uint32_t free_heap = ESP.getFreeHeap();
    serial_log("[ui_font] diag: free heap=%lu", (unsigned long)free_heap);

    if (!s_path_lvgl) {
        serial_log("[ui_font] font: LOAD FAILED (file not found) — SimSun fallback");
        return false;
    }

    s_loaded_font = lv_font_load(s_path_lvgl);
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
