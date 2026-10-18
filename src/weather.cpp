#include "weather.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <esp_http_client.h>
#include "sd_card.h"
#include "wifi_net.h"

#include "esp_bsp.h"
#include "serial_log.h"
#include "app_settings.h"
#include "weather_ui.h"

/*
 * Optional SD config (/weather.ini). Without lat/lon the location comes from IP geolocation.
 *   [weather]
 *   lat=40.71
 *   lon=-74.01
 *   name=New York
 *   units=fahrenheit   ; or celsius (default: fahrenheit in the US, celsius elsewhere)
 */
static const char *k_ini_path = "/weather.ini";

static const uint32_t k_refresh_ms = 15UL * 60UL * 1000UL;
static const uint32_t k_retry_ms = 60UL * 1000UL;

struct WeatherConfig {
    bool have_coords;
    float lat;
    float lon;
    String name;
    int units; /* -1 = auto, 0 = celsius, 1 = fahrenheit */
};

static WeatherConfig s_cfg = {false, 0, 0, "", -1};
static TaskHandle_t s_task = nullptr;
static volatile const char *s_status = "Waiting for WiFi";

static void set_status(const char *s) {
    s_status = s;
    serial_log("[weather] %s", s[0] ? s : "OK");
    bsp_display_lock(0);
    weather_ui_on_status();
    bsp_display_unlock();
}

static void load_ini(void) {
    File f = sd_fs().open(k_ini_path, FILE_READ);
    if (!f) {
        serial_log("[weather] %s not found, using IP geolocation", k_ini_path);
        return;
    }
    const String content = f.readString();
    f.close();

    bool have_lat = false, have_lon = false;
    int start = 0;
    while (start < (int)content.length()) {
        int nl = content.indexOf('\n', start);
        if (nl < 0) {
            nl = content.length();
        }
        String line = content.substring(start, nl);
        start = nl + 1;
        line.trim();
        if (line.length() == 0 || line.charAt(0) == ';' || line.charAt(0) == '[') {
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq < 0) {
            continue;
        }
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        const int semi = val.indexOf(';');
        if (semi >= 0) {
            val = val.substring(0, semi);
        }
        key.trim();
        val.trim();
        if (key == "lat") {
            s_cfg.lat = val.toFloat();
            have_lat = true;
        } else if (key == "lon") {
            s_cfg.lon = val.toFloat();
            have_lon = true;
        } else if (key == "name") {
            s_cfg.name = val;
        } else if (key == "units") {
            s_cfg.units = val.equalsIgnoreCase("fahrenheit") ? 1 : val.equalsIgnoreCase("celsius") ? 0 : -1;
        }
    }
    s_cfg.have_coords = have_lat && have_lon;
    serial_log("[weather] %s: coords=%s name=\"%s\" units=%d", k_ini_path, s_cfg.have_coords ? "yes" : "no",
               s_cfg.name.c_str(), s_cfg.units);
}

/** GET url into *out. Returns false (and sets status) on failure. */
/*
 * JSON goes to PSRAM. The forecast is ~10 KB of JSON and thousands of small nodes; as a String
 * body plus a default-allocated document it was a ~25 KB spike in internal RAM on every refresh
 * (everything < 4 KB lands there), which together with BLE drove free internal RAM down to 2 KB.
 */
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void deallocate(void *p) override {
        heap_caps_free(p);
    }
    void *reallocate(void *p, size_t n) override {
        return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};
static PsramJsonAllocator s_json_alloc;

/**
 * ArduinoJson reader over an open esp_http_client: the body is parsed as it arrives (chunked
 * encoding is decoded by the client), through a small buffer instead of one read per byte.
 */
struct HttpBodyReader {
    esp_http_client_handle_t client;
    char buf[256];
    int len = 0;
    int pos = 0;

    bool fill() {
        len = esp_http_client_read(client, buf, sizeof(buf));
        pos = 0;
        return len > 0;
    }
    int read() {
        if (pos >= len && !fill()) {
            return -1;
        }
        return (uint8_t)buf[pos++];
    }
    size_t readBytes(char *dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (pos >= len && !fill()) {
                break;
            }
            const size_t k = std::min(n - got, (size_t)(len - pos));
            memcpy(dst + got, buf + pos, k);
            pos += (int)k;
            got += k;
        }
        return got;
    }
};

/** GET url and parse the JSON straight from the socket (no body buffer). Sets status on failure. */
static bool http_get_json(const String &url, JsonDocument &doc) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.buffer_size_tx = 1024; /* the forecast URL is ~350 characters */
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_status("HTTP init failed");
        return false;
    }
    bool ok = false;
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        serial_log("[weather] GET %s -> %s", url.c_str(), esp_err_to_name(err));
        set_status("Network error");
    } else {
        (void)esp_http_client_fetch_headers(client);
        const int code = esp_http_client_get_status_code(client);
        if (code != 200) {
            serial_log("[weather] GET %s -> HTTP %d", url.c_str(), code);
            set_status("Server error");
        } else {
            HttpBodyReader reader;
            reader.client = client;
            const DeserializationError jerr = deserializeJson(doc, reader);
            if (jerr) {
                serial_log("[weather] JSON error: %s", jerr.c_str());
                set_status("Bad weather data");
            } else {
                ok = true;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

/** Fill lat/lon/name/units from ip-api.com when /weather.ini doesn't give coordinates. */
static bool geolocate(void) {
    JsonDocument doc(&s_json_alloc);
    if (!http_get_json("http://ip-api.com/json/?fields=status,countryCode,city,lat,lon", doc)) {
        return false;
    }
    if (strcmp(doc["status"] | "", "success") != 0) {
        set_status("Location lookup failed");
        return false;
    }
    s_cfg.lat = doc["lat"] | 0.0f;
    s_cfg.lon = doc["lon"] | 0.0f;
    if (s_cfg.name.length() == 0) {
        s_cfg.name = (const char *)(doc["city"] | "");
    }
    if (s_cfg.units < 0) {
        s_cfg.units = strcmp(doc["countryCode"] | "", "US") == 0 ? 1 : 0;
    }
    s_cfg.have_coords = true;
    serial_log("[weather] geolocated: %s (%.2f, %.2f)", s_cfg.name.c_str(), s_cfg.lat, s_cfg.lon);
    return true;
}

static bool fetch_forecast(WeatherData *wd) {
    /* Settings app choice wins; "Auto" falls back to /weather.ini or the IP country. */
    const TempUnits pref = settings_get().units;
    const bool fahrenheit = pref == TempUnits::Fahrenheit || (pref == TempUnits::Auto && s_cfg.units == 1);
    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(s_cfg.lat, 4) +
                 "&longitude=" + String(s_cfg.lon, 4) +
                 "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
                 "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                 "&timezone=auto&forecast_days=" +
                 String(k_weather_days);
    if (fahrenheit) {
        url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";
    }

    JsonDocument doc(&s_json_alloc);
    if (!http_get_json(url, doc)) {
        return false;
    }

    JsonObject cur = doc["current"];
    JsonObject daily = doc["daily"];
    if (cur.isNull() || daily.isNull()) {
        set_status("Bad weather data");
        return false;
    }

    memset(wd, 0, sizeof(*wd));
    wd->fahrenheit = fahrenheit;
    strlcpy(wd->location, s_cfg.name.length() ? s_cfg.name.c_str() : "Weather", sizeof(wd->location));
    wd->temp = cur["temperature_2m"] | 0.0f;
    wd->feels_like = cur["apparent_temperature"] | 0.0f;
    wd->humidity = cur["relative_humidity_2m"] | 0;
    wd->wind = cur["wind_speed_10m"] | 0.0f;
    wd->code = cur["weather_code"] | 0;
    wd->is_day = (cur["is_day"] | 1) != 0;
    for (int i = 0; i < k_weather_days; i++) {
        strlcpy(wd->days[i].date, daily["time"][i] | "", sizeof(wd->days[i].date));
        wd->days[i].code = daily["weather_code"][i] | 0;
        wd->days[i].t_max = daily["temperature_2m_max"][i] | 0.0f;
        wd->days[i].t_min = daily["temperature_2m_min"][i] | 0.0f;
        wd->days[i].pop = daily["precipitation_probability_max"][i] | 0;
    }

    /*
     * Hourly series starts at 00:00 today (local, timezone=auto). Skip hours before the current
     * one by comparing "YYYY-MM-DDTHH" prefixes with current.time, then take the next 24.
     */
    JsonObject hourly = doc["hourly"];
    const char *now_iso = cur["time"] | "";
    JsonArray h_time = hourly["time"];
    wd->hour_count = 0;
    for (size_t i = 0; i < h_time.size() && wd->hour_count < k_weather_hours; i++) {
        const char *ts = h_time[i] | "";
        if (strlen(ts) < 16 || strncmp(ts, now_iso, 13) < 0) {
            continue;
        }
        WeatherHour &h = wd->hours[wd->hour_count++];
        strlcpy(h.time, ts + 11, sizeof(h.time));
        h.code = hourly["weather_code"][i] | 0;
        h.temp = hourly["temperature_2m"][i] | 0.0f;
        h.pop = hourly["precipitation_probability"][i] | 0;
        h.is_day = (hourly["is_day"][i] | 1) != 0;
    }
    const time_t now = time(nullptr);
    wd->updated = now > 1700000000 ? now : 0;
    wd->valid = true;
    serial_log("[weather] %s: %.1f%s, %s, hum %d%%, wind %.1f", wd->location, wd->temp, fahrenheit ? "F" : "C",
               weather_code_text(wd->code), wd->humidity, wd->wind);
    return true;
}

static void weather_task(void *arg) {
    LV_UNUSED(arg);
    load_ini();
    static WeatherData wd; /* static: keep the struct off the task stack */

    for (;;) {
        if (!wifi_net_connected()) {
            if (strcmp((const char *)s_status, "Waiting for WiFi") != 0) {
                set_status("Waiting for WiFi");
            }
            /* Wake early on a refresh request; otherwise re-check WiFi every 2 s. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        set_status("Updating...");
        bool ok = s_cfg.have_coords || geolocate();
        if (ok) {
            if (s_cfg.units < 0) {
                s_cfg.units = 0;
            }
            ok = fetch_forecast(&wd);
        }
        if (ok) {
            bsp_display_lock(0);
            weather_ui_on_data(&wd);
            bsp_display_unlock();
            set_status("");
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? k_refresh_ms : k_retry_ms));
    }
}

void weather_begin(void) {
    if (s_task) {
        return;
    }
    /* 10 KB: HTTP client + JSON parsing; low priority so it never competes with the UI. */
    /* Core 1 (with Arduino loop): core 0 is busy with LVGL redraws and the WiFi stack. */
    xTaskCreatePinnedToCoreWithCaps(weather_task, "weather", 10240, nullptr, 1, &s_task, 1, MALLOC_CAP_SPIRAM); /* PSRAM stack: never touch flash (NVS etc.) here */
}

void weather_refresh(void) {
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

const char *weather_status(void) {
    return (const char *)s_status;
}

const char *weather_code_text(int code) {
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80:
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Storm, hail";
    default: return "Unknown";
    }
}
