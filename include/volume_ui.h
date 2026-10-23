#pragma once

#include <lvgl.h>

/**
 * Speaker volume row for the Music and Radio screens: icon, slider, "50%". Changes apply at once
 * and are saved to settings.ini on release. Meant for the bottom of the screen: presses from the
 * top of the row down to the screen edge reach the slider (they tend to land just below it), so the
 * row's containers are made OVERFLOW_VISIBLE - create it after its parents are in place.
 */
lv_obj_t *volume_ui_create(lv_obj_t *parent);

/** Show the current volume (it may have changed on another screen). */
void volume_ui_sync(lv_obj_t *row);
