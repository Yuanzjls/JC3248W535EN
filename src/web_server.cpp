#include "web_server.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

#include "airthings.h"
#include "app_settings.h"
#include "clock_ui.h"
#include "esp_bsp.h"
#include "platform_time.h"
#include "serial_log.h"
#include "sysmon.h"
#include "weather.h"
#include "weather_ui.h"
#include "web_page.h"
#include "web_files.h"
#include "wifi_net.h"

static httpd_handle_t s_server = nullptr;
static const size_t k_max_body = 512;          /* settings / location JSON is ~150 bytes */
static const uint32_t k_lock_timeout_ms = 2000; /* UI data is read under the display lock */

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), (ssize_t)out.size());
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

/**
 * Read a JSON request body into doc. Only "Content-Type: application/json" is accepted: a page
 * from another site can't send that cross-origin without a CORS preflight (which we never
 * answer), so random websites open in the user's browser can't change the board's settings.
 */
static bool read_json_body(httpd_req_t *req, JsonDocument &doc) {
    char type[48] = "";
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncmp(type, "application/json", 16) != 0) {
        send_text(req, "415 Unsupported Media Type", "JSON only");
        return false;
    }
    if (req->content_len == 0 || req->content_len > k_max_body) {
        send_text(req, "400 Bad Request", "Body missing or too large");
        return false;
    }
    char body[k_max_body + 1];
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return false; /* connection gone */
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    if (deserializeJson(doc, body, got)) {
        send_text(req, "400 Bad Request", "Invalid JSON");
        return false;
    }
    return true;
}

static const char *units_name(TempUnits u) {
    return u == TempUnits::Celsius ? "celsius" : u == TempUnits::Fahrenheit ? "fahrenheit" : "auto";
}

/* ---------- handlers ---------- */

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_web_page, (ssize_t)(sizeof(k_web_page) - 1));
}

static esp_err_t status_get(httpd_req_t *req) {
    JsonDocument doc;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    if (wifi_net_connected()) {
        char ssid[33], ip[16];
        wifi_net_ssid(ssid, sizeof(ssid));
        wifi_net_ip(ip, sizeof(ip));
        wifi["ssid"] = ssid;
        wifi["ip"] = ip;
        wifi["rssi"] = wifi_net_rssi();
    }
    doc["uptime_s"] = now_ms() / 1000;
    const time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &ti);
        doc["time"] = buf;
    }

    /* Weather, radon and CPU/memory are updated under the display lock by their tasks. */
    if (!bsp_display_lock(k_lock_timeout_ms)) {
        return send_text(req, "503 Service Unavailable", "Busy, try again");
    }
    const SysStats &st = sysmon_get();
    if (st.valid) {
        JsonArray cpu = doc["cpu"].to<JsonArray>();
        cpu.add((int)(st.cpu[0] + 0.5f));
        cpu.add((int)(st.cpu[1] + 0.5f));
        doc["heap_free"] = st.heap_free;
        doc["heap_min"] = st.heap_min;
        doc["psram_free"] = st.psram_free;
    } else {
        doc["heap_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        doc["heap_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        doc["psram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    const WeatherData &wd = weather_ui_data();
    JsonObject w = doc["weather"].to<JsonObject>();
    w["valid"] = wd.valid;
    w["status"] = weather_status();
    if (wd.valid) {
        w["location"] = wd.location;
        w["unit"] = wd.fahrenheit ? "F" : "C";
        w["temp"] = wd.temp;
        w["feels"] = wd.feels_like;
        w["humidity"] = wd.humidity;
        w["wind"] = wd.wind;
        w["text"] = weather_code_text(wd.code);
        w["updated"] = (long long)wd.updated;
    }

    JsonObject a = doc["air"].to<JsonObject>();
    a["configured"] = airthings_configured();
    a["status"] = airthings_status();
    const AirData &ad = airthings_data();
    a["valid"] = ad.valid;
    if (airthings_configured()) {
        a["model"] = airthings_model_name(airthings_serial());
    }
    if (ad.valid) {
        a["radon_st"] = ad.radon_st;
        a["radon_lt"] = ad.radon_lt;
        a["temp_c"] = ad.temp_c;
        a["humidity"] = ad.humidity;
        a["updated"] = (long long)ad.updated;
    }
    bsp_display_unlock();
    return send_json(req, doc);
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    JsonDocument doc;
    const AppSettings &s = settings_get();
    doc["brightness"] = s.brightness;
    doc["units"] = units_name(s.units);
    doc["clock_24h"] = s.clock_24h;
    doc["ambient_min"] = s.ambient_min;
    const char *tz = getenv("TZ");
    doc["timezone"] = tz ? tz : "";
    JsonArray list = doc["timezones"].to<JsonArray>();
    for (int i = 0; i < settings_timezone_count(); i++) {
        JsonObject t = list.add<JsonObject>();
        t["label"] = settings_timezone_label(i);
        t["posix"] = settings_timezone_posix(i);
    }
    WeatherLocation loc;
    weather_get_location(&loc);
    JsonObject l = doc["location"].to<JsonObject>();
    l["manual"] = loc.manual;
    l["known"] = loc.known;
    l["lat"] = loc.lat;
    l["lon"] = loc.lon;
    l["name"] = loc.name;
    return send_json(req, doc);
}

static esp_err_t settings_post(httpd_req_t *req) {
    JsonDocument in;
    if (!read_json_body(req, in)) {
        return ESP_OK; /* response already sent */
    }
    /* Validate everything first, then apply: a bad field changes nothing. */
    int brightness = -1;
    if (in["brightness"].is<int>()) {
        brightness = in["brightness"].as<int>();
        if (brightness < 10 || brightness > 100) {
            return send_text(req, "400 Bad Request", "brightness must be 10..100");
        }
    }
    int units = -1;
    if (in["units"].is<const char *>()) {
        const char *u = in["units"];
        units = !strcmp(u, "celsius") ? (int)TempUnits::Celsius
                : !strcmp(u, "fahrenheit") ? (int)TempUnits::Fahrenheit
                : !strcmp(u, "auto") ? (int)TempUnits::Auto : -2;
        if (units == -2) {
            return send_text(req, "400 Bad Request", "units must be auto, celsius or fahrenheit");
        }
    }
    int ambient = -1;
    if (in["ambient_min"].is<int>()) {
        ambient = in["ambient_min"].as<int>();
        if (ambient != 0 && ambient != 1 && ambient != 3 && ambient != 5 && ambient != 10) {
            return send_text(req, "400 Bad Request", "ambient_min must be 0, 1, 3, 5 or 10");
        }
    }
    const char *tz = nullptr;
    if (in["timezone"].is<const char *>()) {
        const char *want = in["timezone"];
        for (int i = 0; i < settings_timezone_count(); i++) {
            if (strcmp(want, settings_timezone_posix(i)) == 0) {
                tz = settings_timezone_posix(i); /* only zones from the list (stable strings) */
            }
        }
        if (!tz) {
            return send_text(req, "400 Bad Request", "unknown timezone");
        }
    }

    /* Same functions as the Settings app; under the display lock so the UI never sees a change
     * halfway (TZ is read by the clock and status bar on the LVGL task). */
    if (!bsp_display_lock(k_lock_timeout_ms)) {
        return send_text(req, "503 Service Unavailable", "Busy, try again");
    }
    if (brightness >= 0) {
        settings_set_brightness(brightness, true);
    }
    if (units >= 0) {
        settings_set_units((TempUnits)units);
    }
    if (in["clock_24h"].is<bool>()) {
        settings_set_clock_24h(in["clock_24h"].as<bool>());
    }
    if (ambient >= 0) {
        settings_set_ambient_min(ambient);
    }
    if (tz) {
        const char *cur = getenv("TZ");
        if (!cur || strcmp(cur, tz) != 0) {
            clock_ui_set_timezone(tz);
        }
    }
    bsp_display_unlock();
    serial_log("[web] settings changed");
    return send_text(req, "200 OK", "OK");
}

static esp_err_t weather_post(httpd_req_t *req) {
    JsonDocument in;
    if (!read_json_body(req, in)) {
        return ESP_OK;
    }
    WeatherLocation loc = {};
    loc.manual = in["manual"] | false;
    if (loc.manual) {
        if (!in["lat"].is<float>() || !in["lon"].is<float>()) {
            return send_text(req, "400 Bad Request", "lat and lon are required");
        }
        loc.lat = in["lat"].as<float>();
        loc.lon = in["lon"].as<float>();
        strlcpy(loc.name, in["name"] | "", sizeof(loc.name));
    }
    if (!weather_set_location(&loc)) {
        return send_text(req, "400 Bad Request", "Invalid location or SD card write failed");
    }
    return send_text(req, "200 OK", "OK");
}

/* ---------- start ---------- */

void web_server_start(void) {
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;                                /* ArduinoJson + a 512 B body buffer */
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; /* internal RAM is the scarce one */
    cfg.core_id = 1;                                     /* core 0: LVGL task and Wi-Fi */
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true; /* a 5th client closes the least recently used socket */
    cfg.max_uri_handlers = 16;
    cfg.recv_wait_timeout = 10; /* big uploads / downloads over weak Wi-Fi */
    cfg.send_wait_timeout = 10;
    const esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        serial_log("[web] server start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return;
    }
    static const httpd_uri_t k_routes[] = {
        {"/", HTTP_GET, page_get, nullptr},
        {"/api/status", HTTP_GET, status_get, nullptr},
        {"/api/settings", HTTP_GET, settings_get_handler, nullptr},
        {"/api/settings", HTTP_POST, settings_post, nullptr},
        {"/api/weather", HTTP_POST, weather_post, nullptr},
    };
    for (const httpd_uri_t &r : k_routes) {
        httpd_register_uri_handler(s_server, &r);
    }
    web_files_register(s_server);
    serial_log("[web] server started on port %u", (unsigned)cfg.server_port);
}
