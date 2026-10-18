#include "app_settings.h"
#include "platform_time.h"

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include "wifi_net.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_ui.h"
#include "status_bar.h"
#include "sysmon.h"
#include "ui_font.h"
#include "ui_theme.h"

struct TzOption {
    const char *label;
    const char *posix;
};

/* POSIX strings must stay stable: the saved /timezone.ini value is matched against them. */
static const TzOption k_timezones[] = {
    {"UTC", "UTC0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"United Kingdom", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Central Europe", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"India (UTC+5:30)", "IST-5:30"},
    {"China (UTC+8)", "CST-8"},
    {"Japan (UTC+9)", "JST-9"},
    {"Korea (UTC+9)", "KST-9"},
    {"Australia Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};
static const int k_tz_n = sizeof(k_timezones) / sizeof(k_timezones[0]);

int settings_timezone_count(void) {
    return k_tz_n;
}

const char *settings_timezone_label(int i) {
    return (i >= 0 && i < k_tz_n) ? k_timezones[i].label : "";
}

const char *settings_timezone_posix(int i) {
    return (i >= 0 && i < k_tz_n) ? k_timezones[i].posix : "";
}

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_slider = nullptr;
static lv_obj_t *s_slider_val = nullptr;
static lv_obj_t *s_tz_dd = nullptr;
static lv_obj_t *s_units_bm = nullptr;
static lv_obj_t *s_clock_bm = nullptr;
static lv_obj_t *s_about = nullptr;

/* Live System card: one bar per meter, refreshed every second while Settings is visible. */
struct Meter {
    lv_obj_t *bar;
    lv_obj_t *value;
};
static Meter s_cpu0, s_cpu1, s_ram, s_psram;
static lv_obj_t *s_top_tasks = nullptr;
static lv_timer_t *s_sys_timer = nullptr;

/* ---------- callbacks ---------- */

static void brightness_cb(lv_event_t *e) {
    const int v = lv_slider_get_value(s_slider);
    lv_label_set_text_fmt(s_slider_val, "%d%%", v);
    /* Apply live while dragging; write the file once on release. */
    settings_set_brightness(v, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void tz_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint16_t idx = lv_dropdown_get_selected(s_tz_dd);
    if (idx < k_tz_n) {
        clock_ui_set_timezone(k_timezones[idx].posix);
    }
}

static void units_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint16_t sel = lv_btnmatrix_get_selected_btn(s_units_bm);
    if (sel <= 2) {
        settings_set_units((TempUnits)sel);
    }
}

static void clock_fmt_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint16_t sel = lv_btnmatrix_get_selected_btn(s_clock_bm);
    if (sel <= 1) {
        settings_set_clock_24h(sel == 0);
    }
}

/* ---------- layout helpers ---------- */

/** White card holding one setting: label on the left, control on the right. */
static lv_obj_t *add_row(lv_obj_t *list, const char *label) {
    lv_obj_t *row = lv_obj_create(list);
    ui_style_card(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, ui_font_get(), 0);
    lv_obj_set_style_text_color(l, ui_color_text(), 0);
    lv_obj_set_width(l, 120);
    lv_label_set_text(l, label);
    return row;
}

static lv_obj_t *add_segmented(lv_obj_t *row, const char **map, uint16_t selected, lv_event_cb_t cb) {
    lv_obj_t *bm = lv_btnmatrix_create(row);
    lv_btnmatrix_set_map(bm, map);
    lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm, true);
    lv_btnmatrix_set_btn_ctrl(bm, selected, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_height(bm, 34);
    lv_obj_set_flex_grow(bm, 1);
    ui_style_segmented(bm);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return bm;
}

/* ---------- screen ---------- */

static void sync_from_state(void) {
    const AppSettings &s = settings_get();
    lv_slider_set_value(s_slider, s.brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_slider_val, "%d%%", s.brightness);

    const char *cur = getenv("TZ");
    for (int i = 0; i < k_tz_n; i++) {
        if (cur && strcmp(cur, k_timezones[i].posix) == 0) {
            lv_dropdown_set_selected(s_tz_dd, (uint16_t)i);
            break;
        }
    }

    std::string about = "WiFi  ";
    if (wifi_net_connected()) {
        char ssid[33], ip[16];
        wifi_net_ssid(ssid, sizeof(ssid));
        wifi_net_ip(ip, sizeof(ip));
        about += std::string(ssid) + "  (" + ip + ", " + std::to_string(wifi_net_rssi()) + " dBm)";
        about += "\nWeb page  http://" + std::string(ip) + "/";
    } else {
        about += "not connected";
    }
    about += "\nUptime  " + std::to_string(now_ms() / 60000UL) + " min";
    about += "\nBuild  " __DATE__ " " __TIME__;
    lv_label_set_text(s_about, about.c_str());
}

/** Bar colour by load: green, amber above 70 %, red above 90 %. */
static void set_meter(Meter &m, float pct, const char *value) {
    lv_bar_set_value(m.bar, (int32_t)lroundf(pct), LV_ANIM_OFF);
    const lv_palette_t p = pct > 90 ? LV_PALETTE_RED : pct > 70 ? LV_PALETTE_AMBER : LV_PALETTE_GREEN;
    lv_obj_set_style_bg_color(m.bar, lv_palette_main(p), LV_PART_INDICATOR);
    lv_label_set_text(m.value, value);
}

static void update_system_card(void) {
    const SysStats &st = sysmon_get();
    if (!st.valid) {
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.0f%%", st.cpu[0]);
    set_meter(s_cpu0, st.cpu[0], buf);
    snprintf(buf, sizeof(buf), "%.0f%%", st.cpu[1]);
    set_meter(s_cpu1, st.cpu[1], buf);

    const uint32_t ram_used = st.heap_total - st.heap_free;
    snprintf(buf, sizeof(buf), "%lu / %lu KB", (unsigned long)(ram_used / 1024), (unsigned long)(st.heap_total / 1024));
    set_meter(s_ram, 100.0f * ram_used / st.heap_total, buf);

    const uint32_t ps_used = st.psram_total - st.psram_free;
    snprintf(buf, sizeof(buf), "%lu / %lu KB", (unsigned long)(ps_used / 1024), (unsigned long)(st.psram_total / 1024));
    set_meter(s_psram, st.psram_total ? 100.0f * ps_used / st.psram_total : 0, buf);

    lv_label_set_text_fmt(s_top_tasks, "Busiest: %s\nRAM low point %lu KB free, largest block %lu KB", st.top,
                          (unsigned long)(st.heap_min / 1024), (unsigned long)(st.heap_largest / 1024));
}

static void system_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (lv_scr_act() == s_scr) {
        update_system_card();
    }
}

/** One meter row: name | bar | value. */
static Meter add_meter(lv_obj_t *card, const char *name) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, ui_color_text(), 0);
    lv_obj_set_width(l, 64);
    lv_label_set_text(l, name);

    Meter m;
    m.bar = lv_bar_create(row);
    lv_bar_set_range(m.bar, 0, 100);
    lv_obj_set_height(m.bar, 10);
    lv_obj_set_flex_grow(m.bar, 1);
    lv_obj_set_style_bg_color(m.bar, ui_color_track(), LV_PART_MAIN);

    m.value = lv_label_create(row);
    lv_obj_set_style_text_font(m.value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(m.value, ui_color_text_muted(), 0);
    lv_obj_set_width(m.value, 110);
    lv_obj_set_style_text_align(m.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(m.value, "--");
    return m;
}

static void build_screen(void) {
    if (s_scr) {
        return;
    }
    const AppSettings &s = settings_get();

    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scr, ui_color_bg(), 0);
    lv_obj_set_style_pad_all(s_scr, 8, 0);
    lv_obj_set_style_pad_row(s_scr, 6, 0);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    (void)status_bar_create(s_scr, "Settings", true);

    /* Scrollable list of setting cards */
    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    /* Brightness */
    lv_obj_t *row = add_row(list, "Brightness");
    s_slider = lv_slider_create(row);
    lv_slider_set_range(s_slider, 10, 100);
    lv_obj_set_flex_grow(s_slider, 1);
    lv_obj_set_style_pad_hor(row, 18, 0); /* room for the slider knob */
    lv_obj_add_event_cb(s_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_slider, brightness_cb, LV_EVENT_RELEASED, nullptr);
    s_slider_val = lv_label_create(row);
    lv_obj_set_style_text_font(s_slider_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_slider_val, ui_color_text_muted(), 0);
    lv_obj_set_width(s_slider_val, 44);
    lv_obj_set_style_text_align(s_slider_val, LV_TEXT_ALIGN_RIGHT, 0);

    /* Timezone */
    row = add_row(list, "Timezone");
    std::string opts;
    for (int i = 0; i < k_tz_n; i++) {
        if (i) {
            opts += '\n';
        }
        opts += k_timezones[i].label;
    }
    s_tz_dd = lv_dropdown_create(row);
    lv_dropdown_set_options(s_tz_dd, opts.c_str());
    lv_obj_set_flex_grow(s_tz_dd, 1);
    lv_obj_set_style_text_font(s_tz_dd, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_tz_dd), &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(s_tz_dd, tz_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    /* Temperature units */
    row = add_row(list, "Temperature");
    static const char *k_units_map[] = {"Auto", "\xC2\xB0" "C", "\xC2\xB0" "F", ""};
    s_units_bm = add_segmented(row, k_units_map, (uint16_t)s.units, units_cb);

    /* Time format */
    row = add_row(list, "Time format");
    static const char *k_clock_map[] = {"24-hour", "12-hour", ""};
    s_clock_bm = add_segmented(row, k_clock_map, s.clock_24h ? 0 : 1, clock_fmt_cb);

    /* System: live CPU and memory */
    lv_obj_t *sys = lv_obj_create(list);
    ui_style_card(sys);
    lv_obj_set_size(sys, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sys, 10, 0);
    lv_obj_set_style_pad_row(sys, 8, 0);
    lv_obj_set_flex_flow(sys, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sys_title = lv_label_create(sys);
    lv_obj_set_style_text_font(sys_title, ui_font_get(), 0);
    lv_obj_set_style_text_color(sys_title, ui_color_text(), 0);
    lv_label_set_text(sys_title, "System");
    s_cpu0 = add_meter(sys, "Core 0");
    s_cpu1 = add_meter(sys, "Core 1");
    s_ram = add_meter(sys, "RAM");
    s_psram = add_meter(sys, "PSRAM");
    s_top_tasks = lv_label_create(sys);
    lv_obj_set_width(s_top_tasks, LV_PCT(100));
    lv_label_set_long_mode(s_top_tasks, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_top_tasks, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_top_tasks, ui_color_text_muted(), 0);
    lv_label_set_text(s_top_tasks, "Measuring...");
    s_sys_timer = lv_timer_create(system_timer_cb, 1000, nullptr);

    /* About */
    lv_obj_t *about = lv_obj_create(list);
    ui_style_card(about);
    lv_obj_set_size(about, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(about, 10, 0);
    lv_obj_clear_flag(about, LV_OBJ_FLAG_SCROLLABLE);
    s_about = lv_label_create(about);
    lv_obj_set_width(s_about, LV_PCT(100));
    lv_label_set_long_mode(s_about, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_about, ui_font_get(), 0); /* SSID may be CJK */
    lv_obj_set_style_text_color(s_about, ui_color_text_muted(), 0);
    lv_obj_set_style_text_line_space(s_about, 0, 0);
}

void settings_ui_open(void) {
    build_screen();
    sync_from_state();
    update_system_card();
    lv_scr_load(s_scr);
}

void settings_ui_release(void) {
    if (!s_scr || lv_scr_act() == s_scr) {
        return;
    }
    lv_timer_del(s_sys_timer); /* it reads the System card's widgets */
    s_sys_timer = nullptr;
    lv_obj_del(s_scr);
    s_scr = s_slider = s_slider_val = s_tz_dd = s_units_bm = s_clock_bm = s_about = s_top_tasks = nullptr;
    s_cpu0 = s_cpu1 = s_ram = s_psram = Meter{nullptr, nullptr};
}
