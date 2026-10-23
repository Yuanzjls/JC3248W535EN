#include "radio_ui.h"

#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctype.h>
#include <lvgl.h>
#include <string.h>
#include <string>
#include <vector>

#include "audio_player.h"
#include "platform_time.h"
#include "psram_json.h"
#include "sd_text.h"
#include "serial_log.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"
#include "volume_ui.h"
#include "wifi_net.h"

/*
 * Internet radio: now-playing card (station, the song it announces, connection state, prev / play /
 * next, save, volume) next to a station list with three tabs:
 *   Saved - /radio.txt on the SD card, one station per line: "<URL> <name>" (editable on a PC);
 *   Local - the most played MP3 stations in your country (country from the IP address);
 *   Top   - the most played MP3 stations worldwide.
 * Local and Top come from the Radio Browser directory (https://www.radio-browser.info, free, no
 * account), fetched in a background task over plain HTTP. Playing a station from it sends the
 * directory's "click" (its popularity count), as the API asks. Prev / Next step through the list the
 * station was picked from. The last station is kept in /radio_last.txt and shown on open.
 */

struct Station {
    std::string name, url, uuid; /* uuid: Radio Browser id, empty for saved stations */
    std::string info;            /* "pop, rock - 128 kbps" */
};

enum { k_saved, k_local, k_top, k_tabs };

static const char *k_saved_path = "/radio.txt";
static const char *k_last_path = "/radio_last.txt";
static const char *k_user_agent = "JC3248W535EN-radio/1.0 (https://github.com/Yuanzjls/JC3248W535EN)";
static const char *const k_dir_hosts[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info"};
static const size_t k_max_body = 512 * 1024;

/* ---------- UI state ---------- */

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_name = nullptr;
static lv_obj_t *s_info = nullptr;
static lv_obj_t *s_song = nullptr;
static lv_obj_t *s_note = nullptr;
static lv_obj_t *s_play_lbl = nullptr;
static lv_obj_t *s_save_btn = nullptr;
static lv_obj_t *s_vol_row = nullptr;
static lv_obj_t *s_tab_bm = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_timer_t *s_timer = nullptr;

static int s_tab = k_top;
static std::vector<Station> s_lists[k_tabs];
static bool s_loaded[k_tabs];
static std::string s_tab_note[k_tabs];
static std::vector<lv_obj_t *> s_rows; /* rows of the shown tab */
static int s_highlight = -1;

static std::string s_shown_name, s_shown_info, s_shown_song, s_shown_note, s_shown_play, s_shown_save;

static Station s_cur;                  /* station on the card */
static std::vector<Station> s_cur_list; /* list it was picked from (Prev / Next) */
static int s_cur_idx = -1;
static uint32_t s_dir_id = 0;          /* bumped per directory request */

/* ---------- helpers ---------- */

static bool has_non_ascii(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

static const lv_font_t *font_for(const char *text, const lv_font_t *ascii_font) {
    return has_non_ascii(text) ? ui_font_get() : ascii_font;
}

/*
 * Every redraw on this panel is a full frame, so the timer only touches labels whose text changed.
 * Compared with a copy, not lv_label_get_text(): LONG_DOT labels write their "..." into the text.
 */
static void set_cached(lv_obj_t *lbl, std::string *cache, const char *text) {
    if (*cache != text) {
        *cache = text;
        lv_label_set_text(lbl, text);
    }
}

/** Up to two lines, "..." after that (no scrolling: a scrolling label redraws the screen non-stop). */
static void set_two_lines(lv_obj_t *lbl, std::string *cache, const char *text, const lv_font_t *ascii_font) {
    if (*cache == text) {
        return;
    }
    *cache = text;
    const lv_font_t *f = font_for(text, ascii_font);
    lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_label_set_text(lbl, text);
    lv_obj_update_layout(lbl);
    const lv_coord_t max_h = 2 * lv_font_get_line_height(f);
    if (lv_obj_get_height(lbl) > max_h) {
        lv_obj_set_height(lbl, max_h);
    }
}

/** One line with "..." (LONG_DOT needs a fixed height, else it wraps). */
static void one_line(lv_obj_t *lbl, const lv_font_t *font) {
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_height(lbl, lv_font_get_line_height(font));
}

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') {
        a++;
    }
    while (b > a && (unsigned char)s[b - 1] <= ' ') {
        b--;
    }
    return s.substr(a, b - a);
}

/* ---------- saved stations (SD) ---------- */

/** "<URL> <name>" per line; '#' starts a comment line. */
static std::vector<Station> read_station_file(const char *path) {
    std::vector<Station> out;
    std::string text;
    if (!sd_read_text(path, &text)) {
        return out;
    }
    for_each_line(text, [&](const std::string &raw) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') {
            return;
        }
        const size_t sp = line.find_first_of(" \t");
        Station s;
        s.url = line.substr(0, sp);
        s.name = sp == std::string::npos ? s.url : trim(line.substr(sp));
        s.info = "Saved";
        out.push_back(s);
    });
    return out;
}

static void write_station_file(const char *path, const std::vector<Station> &list, const char *header) {
    std::string text = header;
    for (const Station &s : list) {
        text += s.url + " " + s.name + "\n";
    }
    if (!sd_write_text(path, text)) {
        serial_log("[radio] cannot write %s", path);
    }
}

static int saved_index(const std::string &url) {
    for (size_t i = 0; i < s_lists[k_saved].size(); i++) {
        if (s_lists[k_saved][i].url == url) {
            return (int)i;
        }
    }
    return -1;
}

static void save_saved(void) {
    write_station_file(k_saved_path, s_lists[k_saved],
                       "# Saved radio stations: the stream URL, a space, then the name. MP3 streams only.\n");
}

/* ---------- Radio Browser directory (background task) ---------- */

struct DirResult {
    int tab;
    uint32_t id;
    std::vector<Station> list;
    std::string note; /* when the list is empty: why */
};

static SemaphoreHandle_t s_dir_mtx = nullptr;
static TaskHandle_t s_dir_task = nullptr;
static int s_req_tab = -1;
static uint32_t s_req_id = 0;
static std::string s_req_click; /* station uuid to count a play for */
static DirResult *s_dir_done = nullptr;
static std::string s_country;   /* ISO code from the IP address, task-only */

/** Plain-HTTP GET into *body (PSRAM). HTTP status, or -1. */
static int http_get(const std::string &url, std::string *body) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.user_agent = k_user_agent;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    int code = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        code = esp_http_client_get_status_code(client);
        body->clear();
        char buf[1024];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && body->size() < k_max_body) {
            body->append(buf, n);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return code;
}

/** GET a directory path, trying each server. */
static int dir_get(const std::string &path, std::string *body) {
    int code = -1;
    for (const char *host : k_dir_hosts) {
        code = http_get(std::string("http://") + host + path, body);
        if (code == 200) {
            break;
        }
        serial_log("[radio] %s%s -> HTTP %d", host, path.c_str(), code);
    }
    return code;
}

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/** "pop, rock - 128 kbps" from the directory's comma-separated tags and bitrate. */
static std::string station_info(const char *tags, int kbps, const char *country, bool with_country) {
    std::string out;
    std::string t = tags;
    for (int n = 0; n < 2 && !t.empty(); n++) {
        const size_t comma = t.find(',');
        const std::string tag = trim(t.substr(0, comma));
        if (!tag.empty()) {
            out += (out.empty() ? "" : ", ") + tag;
        }
        t = comma == std::string::npos ? "" : t.substr(comma + 1);
    }
    if (kbps > 0) {
        out += (out.empty() ? "" : " - ") + std::to_string(kbps) + " kbps";
    }
    if (with_country && country[0]) {
        out += (out.empty() ? "" : " - ") + std::string(country);
    }
    return out;
}

static DirResult *dir_fetch(int tab, uint32_t id) {
    DirResult *r = new DirResult();
    r->tab = tab;
    r->id = id;
    if (!wifi_net_connected()) {
        r->note = "Connect to Wi-Fi to browse stations";
        return r;
    }
    std::string body;
    std::string query = "/json/stations/search?codec=MP3&order=clickcount&reverse=true&hidebroken=true&limit=60";
    if (tab == k_local) {
        if (s_country.empty() && http_get("http://ip-api.com/json/?fields=countryCode", &body) == 200) {
            JsonDocument doc(&g_psram_json);
            if (!deserializeJson(doc, body)) {
                s_country = doc["countryCode"] | "";
            }
        }
        if (s_country.empty()) {
            r->note = "Couldn't tell which country you are in";
            return r;
        }
        query += "&countrycode=" + url_encode(s_country);
    }
    const uint32_t t0 = now_ms();
    if (dir_get(query, &body) != 200) {
        r->note = "Couldn't reach the station directory";
        return r;
    }
    JsonDocument filter;
    filter[0]["name"] = true;
    filter[0]["url_resolved"] = true;
    filter[0]["stationuuid"] = true;
    filter[0]["tags"] = true;
    filter[0]["bitrate"] = true;
    filter[0]["countrycode"] = true;
    JsonDocument doc(&g_psram_json);
    const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        serial_log("[radio] directory JSON error %s (%u bytes)", err.c_str(), (unsigned)body.size());
        r->note = "Bad answer from the station directory";
        return r;
    }
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        JsonObjectConst o = v.as<JsonObjectConst>();
        Station s;
        s.name = trim(o["name"] | "");
        s.url = trim(o["url_resolved"] | "");
        s.uuid = o["stationuuid"] | "";
        s.info = station_info(o["tags"] | "", o["bitrate"] | 0, o["countrycode"] | "", tab == k_top);
        if (!s.name.empty() && !s.url.empty()) {
            r->list.push_back(s);
        }
    }
    if (r->list.empty()) {
        r->note = "No stations found";
    }
    serial_log("[radio] %s: %u station(s) in %lu ms", tab == k_local ? s_country.c_str() : "top",
               (unsigned)r->list.size(), (unsigned long)(now_ms() - t0));
    return r;
}

static void dir_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_dir_mtx, portMAX_DELAY);
            const int tab = s_req_tab;
            const uint32_t id = s_req_id;
            const std::string click = s_req_click;
            s_req_tab = -1;
            s_req_click.clear();
            xSemaphoreGive(s_dir_mtx);
            if (tab < 0 && click.empty()) {
                break;
            }
            if (!click.empty() && wifi_net_connected()) {
                std::string body;
                (void)dir_get("/json/url/" + click, &body);
            }
            if (tab >= 0) {
                DirResult *r = dir_fetch(tab, id);
                xSemaphoreTake(s_dir_mtx, portMAX_DELAY);
                delete s_dir_done; /* never taken: superseded */
                s_dir_done = r;
                xSemaphoreGive(s_dir_mtx);
            }
        }
    }
}

static void dir_start(void) {
    if (!s_dir_mtx) {
        s_dir_mtx = xSemaphoreCreateMutex();
        /* PSRAM stack: never touch flash (NVS etc.) here. */
        xTaskCreatePinnedToCoreWithCaps(dir_task, "radio_dir", 8192, nullptr, 1, &s_dir_task, 1, MALLOC_CAP_SPIRAM);
    }
}

static void dir_request(int tab) {
    dir_start();
    xSemaphoreTake(s_dir_mtx, portMAX_DELAY);
    s_req_tab = tab;
    s_req_id = ++s_dir_id;
    xSemaphoreGive(s_dir_mtx);
    xTaskNotifyGive(s_dir_task);
}

static void dir_click(const std::string &uuid) {
    if (uuid.empty()) {
        return;
    }
    dir_start();
    xSemaphoreTake(s_dir_mtx, portMAX_DELAY);
    s_req_click = uuid;
    xSemaphoreGive(s_dir_mtx);
    xTaskNotifyGive(s_dir_task);
}

static DirResult *dir_take(void) {
    if (!s_dir_mtx) {
        return nullptr;
    }
    xSemaphoreTake(s_dir_mtx, portMAX_DELAY);
    DirResult *r = s_dir_done;
    s_dir_done = nullptr;
    xSemaphoreGive(s_dir_mtx);
    return r;
}

/* ---------- list ---------- */

static void play_from_list(int idx);

static void row_cb(lv_event_t *e) {
    play_from_list((int)(uintptr_t)lv_event_get_user_data(e));
}

static void style_row(int i, bool current) {
    if (i < 0 || i >= (int)s_rows.size()) {
        return;
    }
    lv_obj_t *row = s_rows[i];
    const lv_color_t accent = lv_palette_main(LV_PALETTE_INDIGO);
    lv_obj_set_style_bg_opa(row, current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_palette_lighten(LV_PALETTE_INDIGO, 5), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(row, 0), current ? accent : ui_color_text(), 0);
}

static void add_row(size_t idx, const Station &s) {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, ui_color_track(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, ui_color_track(), LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_width(name, LV_PCT(100));
    one_line(name, font_for(s.name.c_str(), &lv_font_montserrat_16));
    lv_label_set_text(name, s.name.c_str());

    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_width(info, LV_PCT(100));
    one_line(info, font_for(s.info.c_str(), &lv_font_montserrat_12));
    lv_obj_set_style_text_color(info, ui_color_text_muted(), 0);
    lv_label_set_text(info, s.info.c_str());

    s_rows.push_back(row);
}

static void list_note(const char *text) {
    lv_obj_t *lbl = lv_label_create(s_list);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, ui_color_text_muted(), 0);
    lv_obj_set_style_pad_all(lbl, 16, 0);
    lv_label_set_text(lbl, text);
}

static void show_tab(int tab) {
    s_tab = tab;
    lv_btnmatrix_set_btn_ctrl(s_tab_bm, tab, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_clean(s_list);
    s_rows.clear();
    s_highlight = -1;
    if (tab == k_saved) {
        s_lists[k_saved] = read_station_file(k_saved_path);
        s_loaded[k_saved] = true;
    } else if (!s_loaded[tab]) {
        list_note("Loading stations...");
        dir_request(tab);
        return;
    }
    const std::vector<Station> &list = s_lists[tab];
    for (size_t i = 0; i < list.size(); i++) {
        add_row(i, list[i]);
    }
    if (list.empty()) {
        list_note(tab == k_saved ? "No saved stations yet.\n\nPlay one from Local or Top and tap " LV_SYMBOL_PLUS
                                   " to save it - or put \"URL name\" lines in /radio.txt."
                                 : s_tab_note[tab].c_str());
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

static void tab_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint16_t sel = lv_btnmatrix_get_selected_btn(s_tab_bm);
    if (sel < k_tabs) {
        show_tab(sel);
    }
}

static void refresh_cb(lv_event_t *e) {
    LV_UNUSED(e);
    s_loaded[s_tab] = false;
    show_tab(s_tab);
}

/* ---------- playing ---------- */

static void play_station(const Station &s) {
    s_cur = s;
    audio_play_stream(s.url, s.name);
    dir_click(s.uuid);
    write_station_file(k_last_path, {s}, "");
    serial_log("[radio] play %s (%s)", s.name.c_str(), s.url.c_str());
}

static void play_from_list(int idx) {
    const std::vector<Station> &list = s_lists[s_tab];
    if (idx < 0 || idx >= (int)list.size()) {
        return;
    }
    s_cur_list = list;
    s_cur_idx = idx;
    play_station(list[idx]);
}

static void step_cb(lv_event_t *e) {
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    const int n = (int)s_cur_list.size();
    if (n == 0) {
        return;
    }
    s_cur_idx = ((s_cur_idx < 0 ? 0 : s_cur_idx + dir) % n + n) % n;
    play_station(s_cur_list[s_cur_idx]);
}

static void play_cb(lv_event_t *e) {
    LV_UNUSED(e);
    AudioStatus st;
    audio_get_status(&st);
    if (st.stream && st.path == s_cur.url) {
        audio_toggle_pause(); /* stop / reconnect */
    } else if (!s_cur.url.empty()) {
        play_station(s_cur);
    }
}

static void save_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_cur.url.empty()) {
        return;
    }
    s_lists[k_saved] = read_station_file(k_saved_path);
    const int i = saved_index(s_cur.url);
    if (i >= 0) {
        s_lists[k_saved].erase(s_lists[k_saved].begin() + i);
    } else {
        Station s = s_cur;
        s.uuid.clear();
        s_lists[k_saved].push_back(s);
    }
    save_saved();
    serial_log("[radio] %s %s", i >= 0 ? "removed" : "saved", s_cur.name.c_str());
    if (s_tab == k_saved) {
        show_tab(k_saved);
    }
}

/** Mirror the player (timer). */
static void refresh(void) {
    AudioStatus st;
    audio_get_status(&st);
    const bool mine = st.stream && st.path == s_cur.url; /* the player is on the card's station */

    set_two_lines(s_name, &s_shown_name, s_cur.url.empty() ? "No station" : s_cur.name.c_str(), &lv_font_montserrat_20);
    set_cached(s_info, &s_shown_info, s_cur.info.c_str());
    set_two_lines(s_song, &s_shown_song, mine && !st.now_playing.empty() ? st.now_playing.c_str() : "",
                  &lv_font_montserrat_14);

    const char *note = "";
    if (s_cur.url.empty()) {
        note = "Pick a station";
    } else if (!mine || st.state != AudioState::Playing) {
        note = mine && st.error ? st.stream_note.c_str() : "Stopped";
    } else if (!st.stream_note.empty()) {
        note = st.stream_note.c_str();
    } else {
        note = "Live";
    }
    set_cached(s_note, &s_shown_note, note);
    set_cached(s_play_lbl, &s_shown_play, mine && st.state == AudioState::Playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);

    const bool saved = !s_cur.url.empty() && saved_index(s_cur.url) >= 0;
    set_cached(lv_obj_get_child(s_save_btn, 0), &s_shown_save, saved ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);

    /* Highlight the station playing in the shown list. */
    int found = -1;
    const std::vector<Station> &list = s_lists[s_tab];
    for (size_t i = 0; i < list.size() && i < s_rows.size() && st.stream; i++) {
        if (list[i].url == st.path) {
            found = (int)i;
            break;
        }
    }
    if (found != s_highlight) {
        style_row(s_highlight, false);
        style_row(found, true);
        s_highlight = found;
    }
}

static void timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (lv_scr_act() != s_scr) {
        return;
    }
    if (DirResult *r = dir_take()) {
        if (r->id == s_dir_id) {
            s_lists[r->tab] = r->list;
            s_tab_note[r->tab] = r->note;
            s_loaded[r->tab] = true;
            if (r->tab == s_tab) {
                show_tab(s_tab);
            }
            s_loaded[r->tab] = !r->list.empty(); /* an empty answer (offline...) is fetched again next time */
        }
        delete r;
    }
    refresh();
}

/* ---------- screen ---------- */

static lv_obj_t *add_plain_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *add_ctrl_btn(lv_obj_t *parent, const char *symbol, lv_coord_t size, lv_event_cb_t cb, void *user) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, size >= 52 ? &lv_font_montserrat_24 : &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_width(lbl, LV_PCT(100));
    one_line(lbl, font);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

static void build_screen(void) {
    if (s_scr) {
        return;
    }
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scr, ui_color_bg(), 0);
    lv_obj_set_style_pad_all(s_scr, 8, 0);
    lv_obj_set_style_pad_row(s_scr, 8, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = status_bar_create(s_scr, "Radio", true);
    (void)status_bar_add_action(bar, LV_SYMBOL_REFRESH, refresh_cb, nullptr);

    lv_obj_t *body = add_plain_row(s_scr);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, 8, 0);

    /* Now playing */
    lv_obj_t *card = lv_obj_create(body);
    ui_style_card(card);
    lv_obj_set_size(card, 240, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *text = lv_obj_create(card);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 3, 0);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);

    s_name = add_label(text, &lv_font_montserrat_20, ui_color_text());
    s_info = add_label(text, &lv_font_montserrat_12, ui_color_text_muted());
    s_song = add_label(text, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_INDIGO));
    lv_obj_set_style_pad_top(s_song, 6, 0);
    s_note = add_label(text, &lv_font_montserrat_12, ui_color_text_muted());

    lv_obj_t *ctrl_row = add_plain_row(card);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    (void)add_ctrl_btn(ctrl_row, LV_SYMBOL_PREV, 40, step_cb, (void *)(intptr_t)-1);
    lv_obj_t *play = add_ctrl_btn(ctrl_row, LV_SYMBOL_PLAY, 52, play_cb, nullptr);
    lv_obj_set_style_bg_color(play, lv_palette_main(LV_PALETTE_INDIGO), 0);
    s_play_lbl = lv_obj_get_child(play, 0);
    (void)add_ctrl_btn(ctrl_row, LV_SYMBOL_NEXT, 40, step_cb, (void *)(intptr_t)1);
    s_save_btn = add_ctrl_btn(ctrl_row, LV_SYMBOL_PLUS, 40, save_cb, nullptr);
    lv_obj_set_style_bg_color(s_save_btn, ui_color_track(), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_save_btn, 0), ui_color_text(), 0);

    s_vol_row = volume_ui_create(card);

    /* Stations: [Saved | Local | Top] over the list */
    lv_obj_t *right = lv_obj_create(body);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    static const char *k_tab_map[] = {"Saved", "Local", "Top", ""};
    s_tab_bm = lv_btnmatrix_create(right);
    lv_btnmatrix_set_map(s_tab_bm, k_tab_map);
    lv_btnmatrix_set_btn_ctrl_all(s_tab_bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(s_tab_bm, true);
    lv_obj_set_size(s_tab_bm, LV_PCT(100), 32);
    ui_style_segmented(s_tab_bm);
    lv_obj_add_event_cb(s_tab_bm, tab_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_list = lv_obj_create(right);
    ui_style_card(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_timer = lv_timer_create(timer_cb, 250, nullptr);
}

void radio_ui_open(void) {
    build_screen();
    /* The card shows what plays now, else the last station (not started: it's a choice to listen). */
    AudioStatus st;
    audio_get_status(&st);
    if (st.stream && !st.path.empty()) {
        if (s_cur.url != st.path) {
            s_cur = Station{st.title, st.path, "", ""};
        }
    } else if (s_cur.url.empty()) {
        const std::vector<Station> last = read_station_file(k_last_path);
        if (!last.empty()) {
            s_cur = last[0];
            s_cur.info.clear();
        }
    }
    s_lists[k_saved] = read_station_file(k_saved_path);
    show_tab(s_lists[k_saved].empty() ? (s_tab == k_saved ? k_top : s_tab) : s_tab);
    volume_ui_sync(s_vol_row);
    lv_scr_load(s_scr);
    refresh();
}

void radio_ui_release(void) {
    if (!s_scr || lv_scr_act() == s_scr) {
        return;
    }
    lv_timer_del(s_timer);
    lv_obj_del(s_scr);
    s_scr = s_name = s_info = s_song = s_note = s_play_lbl = s_save_btn = s_vol_row = s_tab_bm = s_list = nullptr;
    s_timer = nullptr;
    s_rows.clear();
    s_highlight = -1;
    s_shown_name.clear();
    s_shown_info.clear();
    s_shown_song.clear();
    s_shown_note.clear();
    s_shown_play.clear();
    s_shown_save.clear();
    s_dir_id++; /* a directory answer still on its way is dropped */
    for (int i = 0; i < k_tabs; i++) {
        s_lists[i].clear();
        s_lists[i].shrink_to_fit();
        s_loaded[i] = false;
    }
    /* s_cur, s_cur_list and s_tab stay: reopening shows the same station and tab. */
}
