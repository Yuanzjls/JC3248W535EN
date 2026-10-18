#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "display.h"
#include "sd_text.h"
#include "serial_log.h"
#include "weather.h"

/*
 * /settings.ini on SD:
 *   [settings]
 *   brightness=80
 *   units=auto        ; auto | celsius | fahrenheit
 *   clock=24h         ; 24h | 12h
 * Timezone is kept separately in /timezone.ini (timezone_ini.cpp).
 */
static const char *k_path = "/settings.ini";
static const int k_min_brightness = 10; /* never let the slider black out the screen */

static AppSettings s_settings = {100, TempUnits::Auto, true};

static const char *units_name(TempUnits u) {
    return u == TempUnits::Celsius ? "celsius" : u == TempUnits::Fahrenheit ? "fahrenheit" : "auto";
}

static int clamp_brightness(int v) {
    return v < k_min_brightness ? k_min_brightness : v > 100 ? 100 : v;
}

static void save(void) {
    char text[160];
    snprintf(text, sizeof(text), "[settings]\nbrightness=%d\nunits=%s\nclock=%s\n", s_settings.brightness,
             units_name(s_settings.units), s_settings.clock_24h ? "24h" : "12h");
    if (!sd_write_text(k_path, text)) {
        serial_log("[settings] cannot write %s", k_path);
        return;
    }
    serial_log("[settings] saved %s", k_path);
}

void settings_load(void) {
    std::string content;
    if (!sd_read_text(k_path, &content)) {
        serial_log("[settings] %s not found, using defaults", k_path);
        return;
    }
    ini_parse(content, [](const std::string &, const std::string &key, const std::string &val) {
        if (key == "brightness") {
            s_settings.brightness = clamp_brightness(atoi(val.c_str()));
        } else if (key == "units") {
            s_settings.units = str_ieq(val, "celsius")      ? TempUnits::Celsius
                               : str_ieq(val, "fahrenheit") ? TempUnits::Fahrenheit
                                                            : TempUnits::Auto;
        } else if (key == "clock") {
            s_settings.clock_24h = !str_ieq(val, "12h");
        }
    });
    serial_log("[settings] brightness=%d units=%s clock=%s", s_settings.brightness, units_name(s_settings.units),
               s_settings.clock_24h ? "24h" : "12h");
}

const AppSettings &settings_get(void) {
    return s_settings;
}

void settings_set_brightness(int percent, bool save_now) {
    s_settings.brightness = clamp_brightness(percent);
    (void)bsp_display_brightness_set(s_settings.brightness);
    if (save_now) {
        save();
    }
}

void settings_set_units(TempUnits units) {
    if (units == s_settings.units) {
        return;
    }
    s_settings.units = units;
    save();
    weather_refresh();
}

void settings_set_clock_24h(bool on) {
    if (on == s_settings.clock_24h) {
        return;
    }
    s_settings.clock_24h = on;
    save();
}

void settings_format_hm(char *buf, size_t n, const struct tm *ti, bool with_ampm) {
    if (s_settings.clock_24h) {
        snprintf(buf, n, "%02d:%02d", ti->tm_hour, ti->tm_min);
        return;
    }
    const int h12 = ti->tm_hour % 12 == 0 ? 12 : ti->tm_hour % 12;
    if (with_ampm) {
        snprintf(buf, n, "%d:%02d %s", h12, ti->tm_min, ti->tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, n, "%d:%02d", h12, ti->tm_min);
    }
}
