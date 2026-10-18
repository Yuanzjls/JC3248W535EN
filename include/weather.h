#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int k_weather_days = 7;   /* today + 6 */
static const int k_weather_hours = 24; /* starting at the current hour */

struct WeatherDay {
    char date[11]; /* "YYYY-MM-DD" */
    int code;      /* WMO weather code */
    float t_max;
    float t_min;
    int pop; /* max precipitation probability, % */
};

struct WeatherHour {
    char time[6]; /* local "HH:MM" */
    int code;
    float temp;
    int pop; /* precipitation probability, % */
    bool is_day;
};

struct WeatherData {
    bool valid;
    char location[48];
    bool fahrenheit;
    float temp;
    float feels_like;
    int humidity;
    float wind;
    int code;
    bool is_day;
    WeatherDay days[k_weather_days];
    WeatherHour hours[k_weather_hours];
    int hour_count; /* valid entries in hours[] */
    time_t updated; /* UNIX time of last successful fetch (0 if clock not synced) */
};

/**
 * Start the background weather task. It waits for WiFi, resolves the location
 * (/weather.ini on SD, else IP geolocation), then fetches from Open-Meteo every 15 min.
 * Call once from setup() after the SD card is mounted.
 */
void weather_begin(void);

/** Ask the task to fetch now (safe from any task, e.g. an LVGL button callback). */
void weather_refresh(void);

/** Where the forecast is for. */
struct WeatherLocation {
    bool manual;   /* true: from /weather.ini; false: IP geolocation */
    bool known;    /* coordinates resolved (geolocation may still be pending) */
    float lat;
    float lon;
    char name[48];
};

/** Current location (safe from any task). */
void weather_get_location(WeatherLocation *out);

/**
 * Use a fixed location (manual = true: lat/lon/name, saved to /weather.ini) or go back to IP
 * geolocation (manual = false: /weather.ini removed). Re-fetches right away. Any task.
 * False if the file can't be written or the coordinates are out of range.
 */
bool weather_set_location(const WeatherLocation *loc);

/** Short status for the UI ("Waiting for WiFi", "Updating...", error text, or ""). */
const char *weather_status(void);

/** Human-readable name for a WMO weather code ("Partly cloudy", "Rain", ...). */
const char *weather_code_text(int code);
