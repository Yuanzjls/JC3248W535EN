#include "settings.h"

#include <Arduino.h>
#include <SD.h>
#include <stdio.h>

#include "display.h"
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

static void save(void) {
    File f = SD.open(k_path, FILE_WRITE);
    if (!f) {
        serial_log("[settings] cannot write %s", k_path);
        return;
    }
    f.println("[settings]");
    f.printf("brightness=%d\n", s_settings.brightness);
    f.printf("units=%s\n", units_name(s_settings.units));
    f.printf("clock=%s\n", s_settings.clock_24h ? "24h" : "12h");
    f.close();
    serial_log("[settings] saved %s", k_path);
}

void settings_load(void) {
    File f = SD.open(k_path, FILE_READ);
    if (!f) {
        serial_log("[settings] %s not found, using defaults", k_path);
        return;
    }
    const String content = f.readString();
    f.close();

    int start = 0;
    while (start < (int)content.length()) {
        int nl = content.indexOf('\n', start);
        if (nl < 0) {
            nl = content.length();
        }
        String line = content.substring(start, nl);
        start = nl + 1;
        const int semi = line.indexOf(';');
        if (semi >= 0) {
            line = line.substring(0, semi);
        }
        line.trim();
        const int eq = line.indexOf('=');
        if (eq < 0) {
            continue;
        }
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        if (key == "brightness") {
            s_settings.brightness = constrain(val.toInt(), k_min_brightness, 100);
        } else if (key == "units") {
            s_settings.units = val.equalsIgnoreCase("celsius")      ? TempUnits::Celsius
                               : val.equalsIgnoreCase("fahrenheit") ? TempUnits::Fahrenheit
                                                                    : TempUnits::Auto;
        } else if (key == "clock") {
            s_settings.clock_24h = !val.equalsIgnoreCase("12h");
        }
    }
    serial_log("[settings] brightness=%d units=%s clock=%s", s_settings.brightness, units_name(s_settings.units),
               s_settings.clock_24h ? "24h" : "12h");
}

const AppSettings &settings_get(void) {
    return s_settings;
}

void settings_set_brightness(int percent, bool save_now) {
    s_settings.brightness = constrain(percent, k_min_brightness, 100);
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
