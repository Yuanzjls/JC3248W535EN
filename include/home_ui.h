#pragma once

#include <lvgl.h>

/** Build the home (launcher) screen and make it the active screen. Call under bsp_display_lock. */
void home_ui_init(void);

/** Return to the home screen (call from LVGL context / under bsp_display_lock). */
void home_ui_show(void);

/** Add a "Home" button to an app's header; tapping it returns to the home screen. */
lv_obj_t *home_ui_add_home_button(lv_obj_t *parent);
