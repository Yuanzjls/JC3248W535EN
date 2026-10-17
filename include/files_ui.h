#pragma once

/** Show the SD card file explorer (LVGL context / under bsp_display_lock). */
void files_ui_open(void);

/** Delete the Files screens (keeps the current folder; rebuilt on next open). LVGL context. */
void files_ui_release(void);
