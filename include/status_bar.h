#pragma once

#include <lvgl.h>

/**
 * Shared top bar for every screen:
 *   [Home] Title ............ [actions] 72°  WiFi  14:05
 * Updated once a second (and immediately when its screen is loaded). Call under bsp_display_lock.
 */
lv_obj_t *status_bar_create(lv_obj_t *parent, const char *title, bool show_home);

void status_bar_set_title(lv_obj_t *bar, const char *title);

/** Add an app button (e.g. refresh) to the bar, left of the status info. Returns the button. */
lv_obj_t *status_bar_add_action(lv_obj_t *bar, const char *symbol, lv_event_cb_t cb, void *user_data);
