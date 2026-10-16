#include "clock_ui.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <WiFi.h>
#include <lvgl.h>

#include "esp_bsp.h"
#include "home_ui.h"
#include "serial_log.h"
#include "time_sync.h"
#include "timezone_ini.h"
#include "ui_font.h"
#include "ui_theme.h"

static String s_dd_options;

static bool s_tz_ready = false;

static lv_obj_t *s_clock_scr = nullptr;
static lv_obj_t *s_meter = nullptr;
static lv_obj_t *s_time_lbl = nullptr;
static lv_obj_t *s_clock_title = nullptr;
static lv_timer_t *s_clock_timer = nullptr;

static lv_meter_indicator_t *s_ind_sec = nullptr;
static lv_meter_indicator_t *s_ind_min = nullptr;
static lv_meter_indicator_t *s_ind_hour = nullptr;

static bool s_clock_built = false;

/** Full-screen timezone settings (opened by swipe-right on clock). */
static lv_obj_t *s_settings_scr = nullptr;
static lv_obj_t *s_settings_dd = nullptr;

static const char *const k_tz_labels[] = {
    "UTC",
    "China (UTC+8)",
    "Japan (UTC+9)",
    "Korea (UTC+9)",
    "Central Europe",
    "United Kingdom",
    "US Eastern",
    "US Pacific",
};

static const char *const k_tz_posix[] = {
    "UTC0",
    "CST-8",
    "JST-9",
    "KST-9",
    "CET-1CEST,M3.5.0/2,M10.5.0/3",
    "GMT0BST,M3.5.0/1,M10.5.0/2",
    "EST5EDT,M3.2.0,M11.1.0",
    "PST8PDT,M3.2.0,M11.1.0",
};

static const size_t k_tz_n = sizeof(k_tz_posix) / sizeof(k_tz_posix[0]);

static void clock_timer_cb(lv_timer_t *t);
static void build_timezone_settings_screen(void);
static void sync_settings_dropdown(void);
static void open_timezone_settings(void);

static void build_dd_options_string(void) {
    s_dd_options = "";
    for (size_t i = 0; i < k_tz_n; i++) {
        if (i) {
            s_dd_options += '\n';
        }
        s_dd_options += k_tz_labels[i];
    }
}

static void apply_tz_posix(const char *posix) {
    setenv("TZ", posix, 1);
    tzset();
    (void)timezone_ini_save(posix);
    s_tz_ready = true;
    serial_log("[clock] TZ set: %s", posix);
}

static void sync_settings_dropdown(void) {
    if (!s_settings_dd) {
        return;
    }
    const char *cur = getenv("TZ");
    if (!cur) {
        cur = "UTC0";
    }
    for (size_t i = 0; i < k_tz_n; i++) {
        if (strcmp(cur, k_tz_posix[i]) == 0) {
            lv_dropdown_set_selected(s_settings_dd, (uint16_t)i);
            return;
        }
    }
    lv_dropdown_set_selected(s_settings_dd, 0);
}

static void tz_settings_ok_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!s_settings_dd) {
        return;
    }
    const uint32_t idx = lv_dropdown_get_selected(s_settings_dd);
    if (idx >= k_tz_n) {
        return;
    }
    apply_tz_posix(k_tz_posix[idx]);
    if (s_clock_scr) {
        lv_scr_load(s_clock_scr);
    }
}

static void tz_settings_back_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_clock_scr) {
        lv_scr_load(s_clock_scr);
    }
}

static void clock_gesture_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) {
        open_timezone_settings();
    }
}

static void build_timezone_settings_screen(void) {
    if (s_settings_scr) {
        return;
    }

    s_settings_scr = lv_obj_create(nullptr);
    lv_obj_set_size(s_settings_scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_settings_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_settings_scr, ui_color_bg(), 0);

    lv_obj_t *title = lv_label_create(s_settings_scr);
    lv_obj_set_style_text_font(title, ui_font_get(), 0);
    lv_label_set_text(title, "Timezone");
    lv_obj_set_style_text_color(title, ui_color_text(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *hint = lv_label_create(s_settings_scr);
    lv_obj_set_style_text_font(hint, ui_font_get(), 0);
    lv_label_set_text(hint, "Swipe right on clock to open this screen");
    lv_obj_set_style_text_color(hint, ui_color_text_muted(), 0);
    lv_obj_set_width(hint, LV_PCT(90));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *panel = lv_obj_create(s_settings_scr);
    lv_obj_set_size(panel, LV_PCT(92), LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    build_dd_options_string();

    lv_obj_t *lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(lbl, ui_font_get(), 0);
    lv_label_set_text(lbl, "Region");
    lv_obj_set_style_text_color(lbl, ui_color_text(), 0);

    s_settings_dd = lv_dropdown_create(panel);
    lv_obj_set_width(s_settings_dd, LV_PCT(100));
    lv_dropdown_set_options(s_settings_dd, s_dd_options.c_str());
    lv_obj_set_style_text_font(s_settings_dd, ui_font_get(), LV_PART_MAIN);
    {
        lv_obj_t *lst = lv_dropdown_get_list(s_settings_dd);
        if (lst) {
            lv_obj_set_style_text_font(lst, ui_font_get(), LV_PART_MAIN);
        }
    }

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_ok = lv_btn_create(row);
    lv_obj_set_flex_grow(btn_ok, 1);
    lv_obj_set_height(btn_ok, 44);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "Save");
    lv_obj_set_style_text_font(lbl_ok, ui_font_get(), 0);
    lv_obj_center(lbl_ok);
    lv_obj_add_event_cb(btn_ok, tz_settings_ok_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_back = lv_btn_create(row);
    lv_obj_set_flex_grow(btn_back, 1);
    lv_obj_set_height(btn_back, 44);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_font(lbl_back, ui_font_get(), 0);
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, tz_settings_back_cb, LV_EVENT_CLICKED, nullptr);
}

static void open_timezone_settings(void) {
    if (!s_clock_scr) {
        return;
    }
    bsp_display_lock(0);
    build_timezone_settings_screen();
    sync_settings_dropdown();
    lv_scr_load(s_settings_scr);
    bsp_display_unlock();
}

static void build_clock_screen(void) {
    if (s_clock_built || s_clock_scr) {
        return;
    }

    s_clock_scr = lv_obj_create(nullptr);
    lv_obj_set_size(s_clock_scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_clock_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_clock_scr, ui_color_bg(), 0);
    lv_obj_add_event_cb(s_clock_scr, clock_gesture_cb, LV_EVENT_GESTURE, nullptr);

    s_clock_title = lv_label_create(s_clock_scr);
    lv_obj_set_style_text_font(s_clock_title, ui_font_get(), 0);
    lv_label_set_text(s_clock_title, "Clock");
    lv_obj_set_style_text_color(s_clock_title, ui_color_text(), 0);
    lv_obj_align(s_clock_title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(s_clock_title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *home_btn = home_ui_add_home_button(s_clock_scr);
    lv_obj_align(home_btn, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_disp_t *disp = lv_disp_get_default();
    const lv_coord_t hres = lv_disp_get_hor_res(disp);
    const lv_coord_t vres = lv_disp_get_ver_res(disp);
    const lv_coord_t ms = (hres < vres ? hres : vres) * 55 / 100;

    s_meter = lv_meter_create(s_clock_scr);
    lv_obj_set_size(s_meter, ms, ms);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, -8);
    lv_obj_add_flag(s_meter, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_meter_scale_t *sc = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_ticks(s_meter, sc, 61, 2, 8, ui_color_tick());
    lv_meter_set_scale_range(s_meter, sc, 0, 60, 360, 270);

    lv_meter_scale_t *sc12 = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_ticks(s_meter, sc12, 12, 0, 0, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(s_meter, sc12, 1, 3, 14, ui_color_text(), 10);
    lv_meter_set_scale_range(s_meter, sc12, 1, 12, 330, 300);

    s_ind_sec = lv_meter_add_needle_line(s_meter, sc, 2, lv_palette_main(LV_PALETTE_RED), -10);
    /* Light theme: dark hour hand, blue minute hand, red second hand. */
    s_ind_min = lv_meter_add_needle_line(s_meter, sc, 4, lv_palette_main(LV_PALETTE_BLUE), -16);
    s_ind_hour = lv_meter_add_needle_line(s_meter, sc, 6, ui_color_text(), -24);

    s_time_lbl = lv_label_create(s_clock_scr);
    lv_obj_set_style_text_font(s_time_lbl, ui_font_get(), 0);
    lv_obj_set_style_text_color(s_time_lbl, ui_color_text(), 0);
    lv_label_set_text(s_time_lbl, "Waiting for time sync (connect WiFi)");
    lv_obj_align(s_time_lbl, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_add_flag(s_time_lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *swipe_hint = lv_label_create(s_clock_scr);
    lv_obj_set_style_text_font(swipe_hint, ui_font_get(), 0);
    lv_label_set_text(swipe_hint, "Swipe right -> timezone");
    lv_obj_set_style_text_color(swipe_hint, ui_color_text_muted(), 0);
    lv_obj_align(swipe_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(swipe_hint, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_clock_timer = lv_timer_create(clock_timer_cb, 250, nullptr);

    s_clock_built = true;
    serial_log("[clock] analog clock screen built");
}

static void clock_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (!s_meter || !s_time_lbl || lv_scr_act() != s_clock_scr) {
        return;
    }
    time_t now = time(nullptr);
    if (now < 1700000000) {
        return;
    }
    struct tm ti;
    localtime_r(&now, &ti);
    const int sec = ti.tm_sec;
    const int min = ti.tm_min;
    const int hr = ti.tm_hour % 12;
    const int hour_val = hr * 5 + min / 12;

    lv_meter_set_indicator_value(s_meter, s_ind_sec, sec);
    lv_meter_set_indicator_value(s_meter, s_ind_min, min);
    lv_meter_set_indicator_value(s_meter, s_ind_hour, hour_val);

    char buf[40];
    (void)snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                   ti.tm_hour, ti.tm_min, ti.tm_sec);
    lv_label_set_text(s_time_lbl, buf);
}

void clock_ui_init(void) {
    String tz;
    if (timezone_ini_load(&tz)) {
        setenv("TZ", tz.c_str(), 1);
        tzset();
        s_tz_ready = true;
        serial_log("[clock] TZ restored: %s", tz.c_str());
    }
}

void clock_ui_on_wifi_connected(void) {
    time_sync_on_wifi_connected();
}

void clock_ui_on_wifi_lost(void) {
    time_sync_on_wifi_disconnected();
}

void clock_ui_open(void) {
    build_clock_screen();
    if (!s_tz_ready) {
        /* First use: pick a timezone; Save/Back on that screen returns to the clock. */
        open_timezone_settings();
        return;
    }
    lv_scr_load(s_clock_scr);
}

void clock_ui_loop(void) {
    time_sync_poll();
}
