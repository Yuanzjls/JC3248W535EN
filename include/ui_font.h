#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mount SD card (SPI) before LVGL init. Uses /sd as VFS mount (must match lv_conf LV_FS_POSIX_PATH).
 */
bool ui_font_mount_sd(void);

/**
 * Load fonts/UI_FONT_16.bin via LVGL FS (call after lv_init, under bsp_display_lock).
 * On failure, ui_font_get() falls back to built-in CJK font.
 */
bool ui_font_load_from_sd(void);

/** Active UI font: SD binary if loaded, else lv_font_simsun_16_cjk. */
const lv_font_t *ui_font_get(void);

#ifdef __cplusplus
}
#endif
