#pragma once

#include <lvgl.h>

/*
 * Shared light-theme colors for the custom screens (home, clock, timezone).
 * Widgets not styled here (buttons, lists, dropdowns, keyboard) use LVGL's default
 * light theme (LV_THEME_DEFAULT_DARK 0 in lv_conf.h), so everything matches.
 */
static inline lv_color_t ui_color_bg(void) { return lv_color_hex(0xF2F4F7); }         /* screen background */
static inline lv_color_t ui_color_text(void) { return lv_color_hex(0x1C2024); }       /* primary text */
static inline lv_color_t ui_color_text_muted(void) { return lv_color_hex(0x5F6B7A); } /* hints, status */
static inline lv_color_t ui_color_tick(void) { return lv_color_hex(0x9AA4B0); }       /* clock minor ticks */
