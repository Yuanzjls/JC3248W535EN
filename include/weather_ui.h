#pragma once

#include <lvgl.h>

#include "weather.h"

/** Show the Weather app (LVGL context / under bsp_display_lock). */
void weather_ui_open(void);

/** New data from the weather task. Caller holds bsp_display_lock. */
void weather_ui_on_data(const WeatherData *wd);

/** weather_status() changed. Caller holds bsp_display_lock. */
void weather_ui_on_status(void);

/** Short text for the home-screen tile, e.g. "72°" or "--°" (LVGL context). */
const char *weather_ui_tile_text(void);

/** Delete the Weather screen (keeps the data; rebuilt on next open). LVGL context. */
void weather_ui_release(void);
