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
static inline lv_color_t ui_color_surface(void) { return lv_color_white(); }          /* cards, selected pill */
static inline lv_color_t ui_color_track(void) { return lv_color_hex(0xE2E6EB); }      /* segmented-control track */

/** Style a checkable one-of-N lv_btnmatrix as a pill segmented control (grey track, white selected pill). */
void ui_style_segmented(lv_obj_t *bm);

/** White rounded card used for grouped content (settings rows, forecast cards). */
void ui_style_card(lv_obj_t *card);
