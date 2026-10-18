#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SD card is mounted by sd_card_mount() (sd_card.h) before this is used. */

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
