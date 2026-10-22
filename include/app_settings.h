#pragma once

#include <stddef.h>
#include <time.h>

enum class TempUnits : int { Auto = 0, Celsius = 1, Fahrenheit = 2 };

struct AppSettings {
    int brightness;  /* backlight %, 10..100 */
    TempUnits units; /* Auto = from /weather.ini or IP country */
    bool clock_24h;
    int ambient_min; /* ambient screen after this many idle minutes on Home; 0 = off */
    int volume;      /* speaker %, 0..100 */
};

/** Read /settings.ini from SD (defaults if missing). Call after the SD card is mounted. */
void settings_load(void);

const AppSettings &settings_get(void);

/** Apply backlight now; save=true also writes /settings.ini (use false while dragging a slider). */
void settings_set_brightness(int percent, bool save);
/** Apply speaker volume now; save=true also writes /settings.ini (use false while dragging a slider). */
void settings_set_volume(int percent, bool save);
/** Saves and triggers a weather re-fetch in the new units. */
void settings_set_units(TempUnits units);
void settings_set_clock_24h(bool on);
/** Idle minutes on Home before the ambient screen (0 = off; 1, 3, 5 or 10). Saves. */
void settings_set_ambient_min(int minutes);

/** "14:05", or in 12h mode "2:05 PM" (with_ampm) / "2:05" (without). */
void settings_format_hm(char *buf, size_t n, const struct tm *ti, bool with_ampm);

/** Timezones offered in Settings (and on the web page): label like "US Eastern", POSIX TZ string. */
int settings_timezone_count(void);
const char *settings_timezone_label(int i);
const char *settings_timezone_posix(int i);

/** Show the Settings app (LVGL context / under bsp_display_lock). */
void settings_ui_open(void);

/** Delete the Settings screen (rebuilt on next open). LVGL context. */
void settings_ui_release(void);
