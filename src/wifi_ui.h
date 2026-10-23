#pragma once

/** Call once from setup() after display is running (inside bsp_display_lock is OK). */
void wifi_ui_init(void);

/** Show the WiFi app screen (LVGL context / under bsp_display_lock). */
void wifi_ui_open(void);

/** Call from loop(): runs Wi-Fi scan work and connection status updates. */
void wifi_ui_loop(void);
