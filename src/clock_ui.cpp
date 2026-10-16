#include "clock_ui.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl.h>

#include "serial_log.h"
#include "settings.h"
#include "status_bar.h"
#include "time_sync.h"
#include "timezone_ini.h"
#include "ui_font.h"
#include "ui_theme.h"

static bool s_tz_ready = false;

static lv_obj_t *s_clock_scr = nullptr;
static lv_obj_t *s_meter = nullptr;
static lv_obj_t *s_time_lbl = nullptr;

static lv_meter_indicator_t *s_ind_sec = nullptr;
static lv_meter_indicator_t *s_ind_min = nullptr;
static lv_meter_indicator_t *s_ind_hour = nullptr;

static void clock_timer_cb(lv_timer_t *t);

static void build_clock_screen(void) {
    if (s_clock_scr) {
        return;
    }

    s_clock_scr = lv_obj_create(nullptr);
    lv_obj_set_size(s_clock_scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_clock_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_clock_scr, ui_color_bg(), 0);
    lv_obj_set_style_pad_all(s_clock_scr, 8, 0);
    lv_obj_clear_flag(s_clock_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = status_bar_create(s_clock_scr, "Clock", true);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_disp_t *disp = lv_disp_get_default();
    const lv_coord_t hres = lv_disp_get_hor_res(disp);
    const lv_coord_t vres = lv_disp_get_ver_res(disp);
    const lv_coord_t ms = (hres < vres ? hres : vres) * 55 / 100;

    s_meter = lv_meter_create(s_clock_scr);
    lv_obj_set_size(s_meter, ms, ms);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, 4);

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
    lv_obj_align(s_time_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    (void)lv_timer_create(clock_timer_cb, 250, nullptr);
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

    char hm[12];
    settings_format_hm(hm, sizeof(hm), &ti, false);
    char buf[48];
    if (settings_get().clock_24h) {
        (void)snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s:%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, hm,
                       ti.tm_sec);
    } else {
        (void)snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s:%02d %s", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                       hm, ti.tm_sec, ti.tm_hour < 12 ? "AM" : "PM");
    }
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

void clock_ui_set_timezone(const char *posix) {
    setenv("TZ", posix, 1);
    tzset();
    (void)timezone_ini_save(posix);
    s_tz_ready = true;
    serial_log("[clock] TZ set: %s", posix);
}

void clock_ui_on_wifi_connected(void) {
    time_sync_on_wifi_connected();
}

void clock_ui_on_wifi_lost(void) {
    time_sync_on_wifi_disconnected();
}

void clock_ui_open(void) {
    if (!s_tz_ready) {
        /* First use: pick a timezone in Settings, then come back to the clock. */
        settings_ui_open();
        return;
    }
    build_clock_screen();
    lv_scr_load(s_clock_scr);
}

void clock_ui_loop(void) {
    time_sync_poll();
}
