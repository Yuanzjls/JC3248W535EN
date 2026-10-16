#include "weather_ui.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "home_ui.h"
#include "serial_log.h"
#include "ui_font.h"
#include "ui_theme.h"

static const int k_fc_cards = 4; /* forecast cards: days 1..4 (day 0 is shown in the main panel) */
static const lv_coord_t k_main_icon = 92;
static const lv_coord_t k_fc_icon = 44;

static WeatherData s_data;
static char s_tile_text[12] = "--\xC2\xB0";

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_icon = nullptr;
static lv_obj_t *s_temp = nullptr;
static lv_obj_t *s_cond = nullptr;
static lv_obj_t *s_details = nullptr;
static lv_obj_t *s_status = nullptr;

struct FcCard {
    lv_obj_t *day;
    lv_obj_t *icon;
    lv_obj_t *range;
};
static FcCard s_fc[k_fc_cards];

/* ---------- icons: built from circles / rounded rects so no image assets are needed ---------- */

enum class Sky { Clear, Partly, Cloudy, Fog, Rain, Snow, Storm };

static Sky sky_for_code(int code) {
    if (code <= 1) return Sky::Clear;
    if (code == 2) return Sky::Partly;
    if (code == 3) return Sky::Cloudy;
    if (code == 45 || code == 48) return Sky::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Sky::Snow;
    if (code >= 95) return Sky::Storm;
    return Sky::Rain; /* drizzle, rain, freezing rain, showers */
}

static lv_obj_t *add_shape(lv_obj_t *box, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t c,
                           lv_coord_t radius) {
    lv_obj_t *o = lv_obj_create(box);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/** Cloud of width w with top-left at (x, y); height is about w * 0.6. */
static void add_cloud(lv_obj_t *box, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_color_t c) {
    const lv_coord_t h = w * 6 / 10;
    add_shape(box, x, y + h * 45 / 100, w, h * 55 / 100, c, LV_RADIUS_CIRCLE);
    add_shape(box, x + w * 12 / 100, y + h * 20 / 100, w * 45 / 100, w * 45 / 100, c, LV_RADIUS_CIRCLE);
    add_shape(box, x + w * 38 / 100, y, w * 52 / 100, w * 52 / 100, c, LV_RADIUS_CIRCLE);
}

static void draw_icon(lv_obj_t *box, lv_coord_t s, int code, bool is_day) {
    lv_obj_clean(box);
    const lv_color_t sun = lv_palette_main(LV_PALETTE_AMBER);
    const lv_color_t moon = lv_palette_lighten(LV_PALETTE_INDIGO, 2);
    const lv_color_t cloud = lv_palette_lighten(LV_PALETTE_BLUE_GREY, 1);
    const lv_color_t dark_cloud = lv_palette_main(LV_PALETTE_BLUE_GREY);
    const lv_color_t rain = lv_palette_main(LV_PALETTE_BLUE);
    const lv_color_t snow = lv_palette_lighten(LV_PALETTE_LIGHT_BLUE, 1);

    switch (sky_for_code(code)) {
    case Sky::Clear: {
        const lv_coord_t d = s * 62 / 100;
        add_shape(box, (s - d) / 2, (s - d) / 2, d, d, is_day ? sun : moon, LV_RADIUS_CIRCLE);
        break;
    }
    case Sky::Partly: {
        const lv_coord_t d = s * 48 / 100;
        add_shape(box, s - d - s / 20, s / 10, d, d, is_day ? sun : moon, LV_RADIUS_CIRCLE);
        add_cloud(box, s / 20, s * 38 / 100, s * 75 / 100, cloud);
        break;
    }
    case Sky::Cloudy:
        add_cloud(box, s / 20, s / 5, s * 9 / 10, cloud);
        break;
    case Sky::Fog:
        add_cloud(box, s / 20, s / 10, s * 9 / 10, cloud);
        add_shape(box, s / 10, s * 72 / 100, s * 8 / 10, s / 14 + 1, cloud, LV_RADIUS_CIRCLE);
        add_shape(box, s / 5, s * 86 / 100, s * 6 / 10, s / 14 + 1, cloud, LV_RADIUS_CIRCLE);
        break;
    case Sky::Rain:
        add_cloud(box, s / 20, 0, s * 9 / 10, cloud);
        for (int i = 0; i < 3; i++) {
            add_shape(box, s * (25 + i * 22) / 100, s * 66 / 100, s / 14 + 1, s / 4, rain, LV_RADIUS_CIRCLE);
        }
        break;
    case Sky::Snow:
        add_cloud(box, s / 20, 0, s * 9 / 10, cloud);
        for (int i = 0; i < 3; i++) {
            const lv_coord_t d = s / 8 + 1;
            add_shape(box, s * (22 + i * 22) / 100, s * (68 + (i % 2) * 14) / 100, d, d, snow, LV_RADIUS_CIRCLE);
        }
        break;
    case Sky::Storm:
        add_cloud(box, s / 20, 0, s * 9 / 10, dark_cloud);
        add_shape(box, s * 46 / 100, s * 62 / 100, s / 9 + 1, s * 34 / 100, sun, 2);
        break;
    }
}

/* ---------- helpers ---------- */

static void fmt_temp(char *buf, size_t n, float t) {
    snprintf(buf, n, "%d\xC2\xB0", (int)lroundf(t));
}

/** "Mon".."Sun" for a "YYYY-MM-DD" date string. */
static const char *weekday_name(const char *date) {
    static const char *const k_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int y = 0, m = 0, d = 0;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        return "--";
    }
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    mktime(&t);
    return k_names[t.tm_wday % 7];
}

static void refresh_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    weather_refresh();
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *make_row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* ---------- screen ---------- */

static void update_status_label(void) {
    if (!s_status) {
        return;
    }
    const char *st = weather_status();
    if (st[0]) {
        lv_label_set_text(s_status, st);
        return;
    }
    if (s_data.valid && s_data.updated) {
        struct tm ti;
        localtime_r(&s_data.updated, &ti);
        lv_label_set_text_fmt(s_status, "Updated %02d:%02d  -  Open-Meteo", ti.tm_hour, ti.tm_min);
    } else {
        lv_label_set_text(s_status, "Open-Meteo");
    }
}

static void update_screen(void) {
    if (!s_scr || !s_data.valid) {
        update_status_label();
        return;
    }
    const WeatherData &wd = s_data;
    char t[16], hi[16], lo[16];

    lv_label_set_text(s_title, wd.location);
    draw_icon(s_icon, k_main_icon, wd.code, wd.is_day);
    fmt_temp(t, sizeof(t), wd.temp);
    lv_label_set_text_fmt(s_temp, "%s%s", t, wd.fahrenheit ? "F" : "C");
    lv_label_set_text(s_cond, weather_code_text(wd.code));

    char feels[16];
    fmt_temp(feels, sizeof(feels), wd.feels_like);
    fmt_temp(hi, sizeof(hi), wd.days[0].t_max);
    fmt_temp(lo, sizeof(lo), wd.days[0].t_min);
    lv_label_set_text_fmt(s_details, "Feels like  %s\nHigh / Low  %s / %s\nHumidity  %d%%\nWind  %d %s", feels, hi, lo,
                          wd.humidity, (int)lroundf(wd.wind), wd.fahrenheit ? "mph" : "km/h");

    for (int i = 0; i < k_fc_cards; i++) {
        const WeatherDay &d = wd.days[i + 1];
        lv_label_set_text(s_fc[i].day, i == 0 ? "Tomorrow" : weekday_name(d.date));
        draw_icon(s_fc[i].icon, k_fc_icon, d.code, true);
        fmt_temp(hi, sizeof(hi), d.t_max);
        fmt_temp(lo, sizeof(lo), d.t_min);
        lv_label_set_text_fmt(s_fc[i].range, "%s / %s", hi, lo);
    }
    update_status_label();
}

static void build_screen(void) {
    if (s_scr) {
        return;
    }
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scr, ui_color_bg(), 0);
    lv_obj_set_style_pad_all(s_scr, 8, 0);
    lv_obj_set_style_pad_row(s_scr, 6, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: home | location | refresh */
    lv_obj_t *header = make_row(s_scr);
    lv_obj_set_style_pad_column(header, 10, 0);
    (void)home_ui_add_home_button(header);
    s_title = make_label(header, ui_font_get(), ui_color_text(), "Weather");
    lv_obj_set_flex_grow(s_title, 1);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_t *refresh = lv_btn_create(header);
    lv_obj_set_size(refresh, 44, 36);
    lv_obj_t *rl = make_label(refresh, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_REFRESH);
    lv_obj_center(rl);
    lv_obj_add_event_cb(refresh, refresh_btn_cb, LV_EVENT_CLICKED, nullptr);

    /* Current conditions: icon | temp + condition | details */
    lv_obj_t *main = make_row(s_scr);
    lv_obj_set_style_pad_column(main, 14, 0);
    s_icon = lv_obj_create(main);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, k_main_icon, k_main_icon);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mid = lv_obj_create(main);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    s_temp = make_label(mid, &lv_font_montserrat_48, ui_color_text(), "--\xC2\xB0");
    s_cond = make_label(mid, ui_font_get(), ui_color_text_muted(), "");

    s_details = make_label(main, &lv_font_montserrat_16, ui_color_text_muted(), "");
    lv_obj_set_flex_grow(s_details, 1);
    lv_obj_set_style_text_line_space(s_details, 4, 0);

    /* 4-day forecast */
    lv_obj_t *fc = make_row(s_scr);
    lv_obj_set_flex_align(fc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < k_fc_cards; i++) {
        lv_obj_t *card = lv_obj_create(fc);
        lv_obj_set_size(card, LV_PCT(23), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        s_fc[i].day = make_label(card, &lv_font_montserrat_16, ui_color_text(), "--");
        s_fc[i].icon = lv_obj_create(card);
        lv_obj_remove_style_all(s_fc[i].icon);
        lv_obj_set_size(s_fc[i].icon, k_fc_icon, k_fc_icon);
        lv_obj_clear_flag(s_fc[i].icon, LV_OBJ_FLAG_SCROLLABLE);
        s_fc[i].range = make_label(card, &lv_font_montserrat_14, ui_color_text_muted(), "-- / --");
    }

    s_status = make_label(s_scr, &lv_font_montserrat_12, ui_color_text_muted(), "");

    update_screen();
}

void weather_ui_open(void) {
    build_screen();
    lv_scr_load(s_scr);
}

void weather_ui_on_data(const WeatherData *wd) {
    s_data = *wd;
    char t[12];
    fmt_temp(t, sizeof(t), wd->temp);
    strlcpy(s_tile_text, t, sizeof(s_tile_text));
    update_screen();
}

void weather_ui_on_status(void) {
    update_status_label();
}

const char *weather_ui_tile_text(void) {
    return s_tile_text;
}
