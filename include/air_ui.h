#pragma once

#include <lvgl.h>

/** Show the Air Quality app (LVGL context / under bsp_display_lock). */
void air_ui_open(void);

/** Delete the screen (data stays in airthings.cpp; rebuilt on next open). LVGL context. */
void air_ui_release(void);

/** Called by the Airthings task under bsp_display_lock when data, results or status change. */
void air_ui_on_update(void);

/** Home tile: radon 24 h value ("48", or "--") and a colour for its level. LVGL context. */
const char *air_ui_tile_text(lv_color_t *color);
