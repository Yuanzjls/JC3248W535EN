#pragma once

/**
 * Show the Radio app (LVGL context / under bsp_display_lock): internet radio stations - saved ones
 * (/radio.txt), popular ones in your country and worldwide (Radio Browser directory) - played
 * through the audio player.
 */
void radio_ui_open(void);

/** Delete the Radio screen (playback continues; rebuilt on next open). LVGL context. */
void radio_ui_release(void);
