#include "volume_ui.h"

#include "app_settings.h"
#include "serial_log.h"
#include "ui_theme.h"

/* Row children: icon, slider, value label. */
enum { k_icon, k_slider, k_value };

/*
 * lv_obj_hit_test() checks the extended click area first and the HIT_TEST event can only narrow
 * it, so the click area is made generous and trimmed here to the slider's width, from the top of
 * the row to the bottom of the screen (the log showed presses at y = 308, under the row).
 */
static const lv_coord_t k_ext_click = 40;

static void hit_test_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    lv_hit_test_info_t *info = (lv_hit_test_info_t *)lv_event_get_param(e);
    lv_area_t a, row;
    lv_obj_get_coords(slider, &a);
    lv_obj_get_coords(lv_obj_get_parent(slider), &row);
    a.x1 -= 12;
    a.x2 += 12;
    a.y1 = row.y1;
    a.y2 = LV_VER_RES - 1;
    info->res = _lv_area_is_point_on(&a, info->point, 0);
}

static void volume_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    const int v = lv_slider_get_value(slider);
    const bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
    lv_label_set_text_fmt(lv_obj_get_child(lv_obj_get_parent(slider), k_value), "%d%%", v);
    settings_set_volume(v, released);
    if (released) {
        serial_log("[ui] volume %d%%", v);
    }
}

lv_obj_t *volume_ui_create(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_color(icon, ui_color_text_muted(), 0);
    lv_label_set_text(icon, LV_SYMBOL_VOLUME_MID);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_ext_click_area(slider, k_ext_click);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(slider, hit_test_cb, LV_EVENT_HIT_TEST, nullptr);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(value, ui_color_text_muted(), 0);
    lv_obj_set_width(value, 32);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

    /* A press below the row is outside its containers too: let the search reach the slider. */
    for (lv_obj_t *p = row; p && lv_obj_get_parent(p); p = lv_obj_get_parent(p)) {
        lv_obj_add_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
    volume_ui_sync(row);
    return row;
}

void volume_ui_sync(lv_obj_t *row) {
    if (!row) {
        return;
    }
    const int v = settings_get().volume;
    lv_slider_set_value(lv_obj_get_child(row, k_slider), v, LV_ANIM_OFF);
    lv_label_set_text_fmt(lv_obj_get_child(row, k_value), "%d%%", v);
}
