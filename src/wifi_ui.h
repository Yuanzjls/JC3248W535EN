#pragma once

/** Call once from setup() after display is running (inside bsp_display_lock is OK). */
void wifi_ui_init(void);

/** Call from Arduino loop() — runs WiFi scan work and connection status updates. */
void wifi_ui_loop(void);
