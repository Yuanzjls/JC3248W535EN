#include "air_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "airthings.h"
#include "app_settings.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"

/*
 * Air Quality app: radon (24 h + long-term), temperature and humidity from an Airthings Wave.
 * Levels use Airthings' own bands: < 100 Bq/m3 good, 100-149 fair, >= 150 poor.
 * 1 pCi/L = 37 Bq/m3.
 */

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_reading = nullptr; /* card with the values (sensor chosen) */
static lv_obj_t *s_radon = nullptr;
static lv_obj_t *s_radon_unit = nullptr;
static lv_obj_t *s_level = nullptr;
static lv_obj_t *s_details = nullptr;
static lv_obj_t *s_find = nullptr;    /* card with "Find sensor" + results */
static lv_obj_t *s_find_hint = nullptr;
static lv_obj_t *s_find_list = nullptr;
static lv_obj_t *s_find_btn = nullptr;
static lv_obj_t *s_footer = nullptr;
static size_t s_shown_found = (size_t)-1; /* rebuild the list only when results change */

static lv_color_t level_color(int bq) {
    if (bq < 0) {
        return ui_color_text_muted();
    }
    return lv_palette_main(bq < 100 ? LV_PALETTE_GREEN : bq < 150 ? LV_PALETTE_AMBER : LV_PALETTE_RED);
}

static const char *level_text(int bq) {
    if (bq < 0) {
        return "Measuring - first value after ~24 h";
    }
    return bq < 100 ? "Good" : bq < 150 ? "Fair - consider ventilating" : "Poor - ventilate, consider mitigation";
}

const char *air_ui_tile_text(lv_color_t *color) {
    static char buf[12];
    const AirData &d = airthings_data();
    if (!d.valid || d.radon_st < 0) {
        *color = lv_palette_main(LV_PALETTE_CYAN);
        return "--";
    }
    *color = level_color(d.radon_st);
    snprintf(buf, sizeof(buf), "%d", d.radon_st);
    return buf;
}

/* ---------- callbacks ---------- */

static void find_cb(lv_event_t *e) {
    LV_UNUSED(e);
    airthings_find();
}

static void refresh_cb(lv_event_t *e) {
    LV_UNUSED(e);
    airthings_read_now();
}

static void use_cb(lv_event_t *e) {
    airthings_choose((size_t)(uintptr_t)lv_event_get_user_data(e));
    s_shown_found = (size_t)-1;
    air_ui_on_update();
}

/* ---------- layout helpers ---------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    ui_style_card(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ---------- update ---------- */

static void rebuild_found_list(void) {
    const std::vector<AirFound> &found = airthings_found();
    if (found.size() == s_shown_found) {
        return;
    }
    s_shown_found = found.size();
    lv_obj_clean(s_find_list);
    for (size_t i = 0; i < found.size(); i++) {
        const AirFound &f = found[i];
        lv_obj_t *row = lv_obj_create(s_find_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_t *icon = label(row, &lv_font_montserrat_20, lv_palette_main(LV_PALETTE_BLUE), LV_SYMBOL_BLUETOOTH);
        LV_UNUSED(icon);
        lv_obj_t *name = label(row, &lv_font_montserrat_16, ui_color_text(), "");
        lv_label_set_text_fmt(name, "%s   SN %lu   %d dBm", airthings_model_name(f.serial),
                              (unsigned long)f.serial, f.rssi);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_t *use = lv_btn_create(row);
        lv_obj_set_height(use, 36);
        lv_obj_t *ul = label(use, &lv_font_montserrat_16, lv_color_white(),
                             f.serial == airthings_serial() ? "In use" : "Use");
        lv_obj_center(ul);
        lv_obj_add_event_cb(use, use_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

void air_ui_on_update(void) {
    if (!s_scr) {
        return;
    }
    const AirData &d = airthings_data();
    const bool configured = airthings_configured();
    const bool busy = airthings_busy();

    /* Reading card: only once a sensor is chosen. */
    if (configured) {
        lv_obj_clear_flag(s_reading, LV_OBJ_FLAG_HIDDEN);
        if (d.valid) {
            const lv_color_t c = level_color(d.radon_st);
            lv_obj_set_style_text_color(s_radon, c, 0);
            if (d.radon_st >= 0) {
                lv_label_set_text_fmt(s_radon, "%d", d.radon_st);
                lv_label_set_text_fmt(s_radon_unit, "Bq/m3   (%.1f pCi/L)\nRadon, 24 h average", d.radon_st / 37.0f);
            } else {
                lv_label_set_text(s_radon, "--");
                lv_label_set_text(s_radon_unit, "Bq/m3\nRadon, 24 h average");
            }
            lv_obj_set_style_text_color(s_level, c, 0);
            lv_label_set_text(s_level, level_text(d.radon_st));

            const bool f = settings_get().units == TempUnits::Fahrenheit;
            char lt[40];
            if (d.radon_lt >= 0) {
                snprintf(lt, sizeof(lt), "%d Bq/m3 (%.1f pCi/L)", d.radon_lt, d.radon_lt / 37.0f);
            } else {
                strlcpy(lt, "not yet", sizeof(lt));
            }
            char s[192];
            int n = snprintf(s, sizeof(s), "Long-term avg   %s\nTemperature   %.1f%s\nHumidity   %.1f %%", lt,
                             f ? d.temp_c * 9 / 5 + 32 : d.temp_c, f ? "\xC2\xB0" "F" : "\xC2\xB0" "C", d.humidity);
            if (d.has_plus && n > 0 && n < (int)sizeof(s)) {
                snprintf(s + n, sizeof(s) - n, "\nCO2   %d ppm    VOC   %d ppb", d.co2_ppm, d.voc_ppb);
            }
            lv_label_set_text(s_details, s);
        } else {
            lv_obj_set_style_text_color(s_radon, ui_color_text_muted(), 0);
            lv_label_set_text(s_radon, "--");
            lv_label_set_text(s_radon_unit, "Bq/m3\nRadon, 24 h average");
            lv_label_set_text(s_level, busy ? "Reading..." : "No reading yet");
            lv_label_set_text(s_details, "");
        }
    } else {
        lv_obj_add_flag(s_reading, LV_OBJ_FLAG_HIDDEN);
    }

    /* Find card: always available, list fills after a scan. */
    lv_label_set_text(s_find_hint, configured ? "Search again to switch sensors."
                                              : "No sensor chosen yet. Search, then tap Use on yours\n"
                                                "(match the serial number on its back).");
    if (busy) {
        lv_obj_add_state(s_find_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_find_btn, LV_STATE_DISABLED);
    }
    rebuild_found_list();

    /* Footer: status, else when/what was read. */
    const char *st = airthings_status();
    if (st[0]) {
        lv_label_set_text(s_footer, st);
    } else if (d.valid) {
        char when[16] = "--:--";
        if (d.updated) {
            struct tm ti;
            localtime_r(&d.updated, &ti);
            settings_format_hm(when, sizeof(when), &ti, true);
        }
        lv_label_set_text_fmt(s_footer, "%s  SN %lu  -  updated %s  -  %d dBm  -  every 5 min",
                              airthings_model_name(d.serial), (unsigned long)d.serial, when, d.rssi);
    } else {
        lv_label_set_text(s_footer, "");
    }
}

/* ---------- screen ---------- */

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

    s_bar = status_bar_create(s_scr, "Air quality", true);
    (void)status_bar_add_action(s_bar, LV_SYMBOL_REFRESH, refresh_cb, nullptr);

    /* Scrollable body */
    lv_obj_t *body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    /* Reading card: big radon number | details */
    s_reading = card(body);
    lv_obj_set_height(s_reading, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_reading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_reading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(s_reading, 18, 0);

    lv_obj_t *left = lv_obj_create(s_reading);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 170, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    s_radon = label(left, &lv_font_montserrat_48, ui_color_text_muted(), "--");
    s_radon_unit = label(left, &lv_font_montserrat_14, ui_color_text_muted(), "");
    s_level = label(left, &lv_font_montserrat_16, ui_color_text_muted(), "");
    lv_obj_set_width(s_level, LV_PCT(100));
    lv_label_set_long_mode(s_level, LV_LABEL_LONG_WRAP);

    s_details = label(s_reading, &lv_font_montserrat_16, ui_color_text(), "");
    lv_obj_set_flex_grow(s_details, 1);
    lv_obj_set_style_text_line_space(s_details, 8, 0);

    /* Find card */
    s_find = card(body);
    lv_obj_set_height(s_find, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_find, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_find, 6, 0);
    lv_obj_t *head = lv_obj_create(s_find);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_find_hint = label(head, &lv_font_montserrat_14, ui_color_text_muted(), "");
    lv_obj_set_flex_grow(s_find_hint, 1);
    s_find_btn = lv_btn_create(head);
    lv_obj_set_height(s_find_btn, 40);
    lv_obj_t *fl = label(s_find_btn, &lv_font_montserrat_16, lv_color_white(), LV_SYMBOL_BLUETOOTH "  Find sensor");
    lv_obj_center(fl);
    lv_obj_add_event_cb(s_find_btn, find_cb, LV_EVENT_CLICKED, nullptr);
    s_find_list = lv_obj_create(s_find);
    lv_obj_remove_style_all(s_find_list);
    lv_obj_set_size(s_find_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_find_list, LV_FLEX_FLOW_COLUMN);

    s_footer = label(s_scr, &lv_font_montserrat_12, ui_color_text_muted(), "");
    lv_obj_set_width(s_footer, LV_PCT(100));
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_DOT);

    s_shown_found = (size_t)-1;
    air_ui_on_update();
}

void air_ui_open(void) {
    build_screen();
    air_ui_on_update();
    lv_scr_load(s_scr);
}

void air_ui_release(void) {
    if (!s_scr || lv_scr_act() == s_scr) {
        return;
    }
    lv_obj_del(s_scr);
    s_scr = s_bar = s_reading = s_radon = s_radon_unit = s_level = s_details = nullptr;
    s_find = s_find_hint = s_find_list = s_find_btn = s_footer = nullptr;
}
