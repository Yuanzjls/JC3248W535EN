#include "status_bar.h"

#include <Arduino.h>
#include "wifi_net.h"
#include <time.h>

#include "home_ui.h"
#include "app_settings.h"
#include "ui_font.h"
#include "ui_theme.h"
#include "weather_ui.h"

struct StatusBar {
    lv_obj_t *bar;
    lv_obj_t *title;
    lv_obj_t *actions;
    lv_obj_t *weather;
    lv_obj_t *wifi;
    lv_obj_t *time;
};

static const int k_max_bars = 16; /* one per screen */
static StatusBar s_bars[k_max_bars];
static int s_bar_count = 0;
static lv_timer_t *s_timer = nullptr;

static StatusBar *find_bar(lv_obj_t *bar) {
    for (int i = 0; i < s_bar_count; i++) {
        if (s_bars[i].bar == bar) {
            return &s_bars[i];
        }
    }
    return nullptr;
}

static void update_bar(StatusBar &b) {
    lv_label_set_text(b.weather, weather_ui_tile_text());

    const bool connected = wifi_net_connected();
    lv_obj_set_style_text_color(b.wifi, connected ? ui_color_text() : ui_color_tick(), 0);

    const time_t now = time(nullptr);
    if (now < 1700000000) {
        lv_label_set_text(b.time, "--:--");
        return;
    }
    struct tm ti;
    localtime_r(&now, &ti);
    char buf[12];
    settings_format_hm(buf, sizeof(buf), &ti, true);
    lv_label_set_text(b.time, buf);
}

/** Only the bar on the visible screen needs refreshing. */
static void status_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    lv_obj_t *act = lv_scr_act();
    for (int i = 0; i < s_bar_count; i++) {
        if (lv_obj_get_screen(s_bars[i].bar) == act) {
            update_bar(s_bars[i]);
        }
    }
}

/** Refresh right away when a screen opens so the bar never shows stale values for a second. */
static void screen_loaded_cb(lv_event_t *e) {
    LV_UNUSED(e);
    status_timer_cb(nullptr);
}

/** Apps free their screens on exit: drop the bar from the registry when it is deleted. */
static void bar_deleted_cb(lv_event_t *e) {
    lv_obj_t *bar = lv_event_get_target(e);
    for (int i = 0; i < s_bar_count; i++) {
        if (s_bars[i].bar == bar) {
            s_bars[i] = s_bars[s_bar_count - 1];
            s_bar_count--;
            return;
        }
    }
}

static lv_obj_t *make_info_label(lv_obj_t *parent, const lv_font_t *font, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color_text(), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *status_bar_create(lv_obj_t *parent, const char *title, bool show_home) {
    if (s_bar_count >= k_max_bars) {
        return nullptr;
    }
    StatusBar &b = s_bars[s_bar_count++];

    b.bar = lv_obj_create(parent);
    lv_obj_remove_style_all(b.bar);
    lv_obj_set_size(b.bar, LV_PCT(100), 40);
    lv_obj_set_flex_flow(b.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b.bar, 10, 0);
    lv_obj_clear_flag(b.bar, LV_OBJ_FLAG_SCROLLABLE);

    if (show_home) {
        (void)home_ui_add_home_button(b.bar);
    }

    b.title = make_info_label(b.bar, ui_font_get(), title);
    lv_obj_set_flex_grow(b.title, 1);
    lv_label_set_long_mode(b.title, LV_LABEL_LONG_DOT);

    b.actions = lv_obj_create(b.bar);
    lv_obj_remove_style_all(b.actions);
    lv_obj_set_size(b.actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b.actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b.actions, 6, 0);

    b.weather = make_info_label(b.bar, &lv_font_montserrat_16, "");
    lv_obj_set_style_text_color(b.weather, ui_color_text_muted(), 0);
    b.wifi = make_info_label(b.bar, &lv_font_montserrat_16, LV_SYMBOL_WIFI);
    b.time = make_info_label(b.bar, &lv_font_montserrat_20, "--:--");

    lv_obj_add_event_cb(lv_obj_get_screen(b.bar), screen_loaded_cb, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(b.bar, bar_deleted_cb, LV_EVENT_DELETE, nullptr);
    if (!s_timer) {
        s_timer = lv_timer_create(status_timer_cb, 1000, nullptr);
    }
    update_bar(b);
    return b.bar;
}

void status_bar_set_title(lv_obj_t *bar, const char *title) {
    StatusBar *b = find_bar(bar);
    if (b) {
        lv_label_set_text(b->title, title);
    }
}

lv_obj_t *status_bar_add_action(lv_obj_t *bar, const char *symbol, lv_event_cb_t cb, void *user_data) {
    StatusBar *b = find_bar(bar);
    if (!b) {
        return nullptr;
    }
    lv_obj_t *btn = lv_btn_create(b->actions);
    lv_obj_set_size(btn, 44, 36);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}
