#pragma once

/**
 * Show the Music app (LVGL context / under bsp_display_lock). Lists the current playlist, or the
 * MP3 files under /music on the SD card when nothing has been played yet.
 */
void music_ui_open(void);

/** Delete the Music screen (playback continues; rebuilt on next open). LVGL context. */
void music_ui_release(void);
