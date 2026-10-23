#pragma once

/*
 * Ambient screen: a full-screen animated sky matching the current weather (clips from
 * /weather/*.mjpeg on the SD card) with the time, date, temperature, conditions, location and
 * radon on top. Opens from Home (tile, or after the idle time set in Settings); a tap goes back.
 */

/** Show it (LVGL context / under bsp_display_lock). False if it is still closing or out of memory. */
bool ambient_ui_open(void);
