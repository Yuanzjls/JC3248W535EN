#pragma once

/** Load saved TZ from NVS (call once at boot, before WiFi). */
void clock_ui_init(void);

void clock_ui_on_wifi_connected(void);
void clock_ui_on_wifi_lost(void);

/** Apply a POSIX TZ string now and save it to /timezone.ini (called from Settings). */
void clock_ui_set_timezone(const char *posix);

/** Show the Clock app (LVGL context / under bsp_display_lock). Opens timezone picker if none saved yet. */
void clock_ui_open(void);

/** NTP polling (call from Arduino loop). */
void clock_ui_loop(void);
