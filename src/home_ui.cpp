#include "home_ui.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "clock_ui.h"
#include "serial_log.h"
#include "settings.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"
#include "weather_ui.h"
#include "wifi_ui.h"

static lv_obj_t *s_home_scr = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_clock_tile_time = nullptr;
static lv_obj_t *s_weather_tile_temp = nullptr;

typedef void (*app_open_fn)(void);

static void home_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    home_ui_show();
}

static void app_tile_cb(lv_event_t *e) {
    app_open_fn open = (app_open_fn)lv_event_get_user_data(e);
    if (open) {
        open();
    }
}

/** Refresh the date title and the live tiles once a second (status bar updates itself). */
static void home_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (lv_scr_act() != s_home_scr) {
        return;
    }
    lv_label_set_text(s_weather_tile_temp, weather_ui_tile_text());

    const time_t now = time(nullptr);
    if (now < 1700000000) {
        status_bar_set_title(s_bar, "Home");
        lv_label_set_text(s_clock_tile_time, "--:--");
        return;
    }
    struct tm ti;
    localtime_r(&now, &ti);
    char buf[24];
    strftime(buf, sizeof(buf), "%a, %b %d", &ti);
    status_bar_set_title(s_bar, buf);
    settings_format_hm(buf, sizeof(buf), &ti, false);
    lv_label_set_text(s_clock_tile_time, buf);
}

/** Rounded app tile: icon (large label) on top, app name below. Returns the icon label. */
static lv_obj_t *add_app_tile(lv_obj_t *parent, const char *icon, const lv_font_t *icon_font, const char *name,
                              lv_color_t color, app_open_fn open) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);

    lv_obj_t *tile = lv_btn_create(col);
    lv_obj_set_size(tile, 96, 96);
    lv_obj_set_style_radius(tile, 24, 0);
    lv_obj_set_style_bg_color(tile, color, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_add_event_cb(tile, app_tile_cb, LV_EVENT_CLICKED, (void *)open);

    lv_obj_t *ic = lv_label_create(tile);
    lv_obj_set_style_text_font(ic, icon_font, 0);
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_label_set_text(ic, icon);
    lv_obj_center(ic);

    lv_obj_t *lbl = lv_label_create(col);
    lv_obj_set_style_text_font(lbl, ui_font_get(), 0);
    lv_obj_set_style_text_color(lbl, ui_color_text(), 0);
    lv_label_set_text(lbl, name);
    return ic;
}

void home_ui_init(void) {
    if (s_home_scr) {
        return;
    }

    s_home_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_home_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_home_scr, ui_color_bg(), 0);
    lv_obj_clear_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_pad_all(s_home_scr, 8, 0);
    s_bar = status_bar_create(s_home_scr, "Home", false);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);

    /* App grid */
    lv_obj_t *apps = lv_obj_create(s_home_scr);
    lv_obj_remove_style_all(apps);
    lv_obj_set_size(apps, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(apps, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_flex_flow(apps, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(apps, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(apps, 16, 0);

    (void)add_app_tile(apps, LV_SYMBOL_WIFI, &lv_font_montserrat_40, "WiFi", lv_palette_main(LV_PALETTE_BLUE),
                       wifi_ui_open);
    s_clock_tile_time = add_app_tile(apps, "--:--", &lv_font_montserrat_26, "Clock",
                                     lv_palette_darken(LV_PALETTE_DEEP_ORANGE, 1), clock_ui_open);
    s_weather_tile_temp = add_app_tile(apps, weather_ui_tile_text(), &lv_font_montserrat_28, "Weather",
                                       lv_palette_main(LV_PALETTE_TEAL), weather_ui_open);
    (void)add_app_tile(apps, LV_SYMBOL_SETTINGS, &lv_font_montserrat_40, "Settings",
                       lv_palette_main(LV_PALETTE_BLUE_GREY), settings_ui_open);

    (void)lv_timer_create(home_timer_cb, 1000, nullptr);

    lv_scr_load(s_home_scr);
    serial_log("[home] home screen shown");
}

void home_ui_show(void) {
    if (s_home_scr && lv_scr_act() != s_home_scr) {
        lv_scr_load(s_home_scr);
    }
}

lv_obj_t *home_ui_add_home_button(lv_obj_t *parent) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 44, 36);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl, LV_SYMBOL_HOME);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, home_btn_cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}
