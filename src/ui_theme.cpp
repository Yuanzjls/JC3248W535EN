#include "ui_theme.h"

void ui_style_segmented(lv_obj_t *bm) {
    lv_obj_set_style_bg_color(bm, ui_color_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bm, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bm, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(bm, 3, LV_PART_MAIN);

    lv_obj_set_style_text_font(bm, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, ui_color_text_muted(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, LV_RADIUS_CIRCLE, LV_PART_ITEMS);

    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, ui_color_surface(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, ui_color_text(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(bm, 6, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(bm, LV_OPA_20, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_ofs_y(bm, 1, LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Light press feedback on either option; no focus outline from touch. */
    lv_obj_set_style_bg_opa(bm, LV_OPA_50, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bm, ui_color_surface(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(bm, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
}

void ui_style_card(lv_obj_t *card) {
    lv_obj_set_style_bg_color(card, ui_color_surface(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
}
