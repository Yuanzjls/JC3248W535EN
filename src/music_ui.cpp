#include "music_ui.h"

#include <dirent.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <algorithm>
#include <string>
#include <string.h>
#include <strings.h>
#include <vector>

#include "app_settings.h"
#include "audio_player.h"
#include "cover_art.h"
#include "platform_time.h"
#include "sd_card.h"
#include "serial_log.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"

/*
 * Music player: now-playing card (cover, title, artist, seek bar, prev / play / next, volume) next
 * to the track list. Playback itself runs in audio_player.cpp and continues after leaving this
 * screen; the screen only mirrors its status (timer) and sends commands. Cover art is decoded in
 * the background (cover_art.cpp) and shown when ready.
 *
 * The list is the player's playlist, or - before anything was played, or after Refresh - every
 * MP3 under /music (subfolders included). Tapping a track plays the list from there.
 */

static const char *k_library_dir = "/music";
static const size_t k_max_tracks = 300;
static const int k_max_depth = 3;
static const int k_cover = 96; /* cover art box, px */

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_cover_img = nullptr;
static lv_obj_t *s_cover_icon = nullptr; /* placeholder while there is no picture */
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_artist = nullptr;
static lv_obj_t *s_info = nullptr;
static lv_obj_t *s_seek = nullptr;
static lv_obj_t *s_pos_lbl = nullptr;
static lv_obj_t *s_dur_lbl = nullptr;
static lv_obj_t *s_play_lbl = nullptr;
static lv_obj_t *s_vol = nullptr;
static lv_obj_t *s_vol_lbl = nullptr;
static lv_timer_t *s_timer = nullptr;

static std::vector<std::string> s_tracks; /* card paths shown in the list */
static std::vector<lv_obj_t *> s_rows;
static std::string s_shown_path;  /* track the card / highlight currently show */
static std::string s_shown_title;
static int s_highlight = -1;

static lv_img_dsc_t s_cover_dsc;
static uint16_t *s_cover_buf = nullptr; /* pixels behind s_cover_dsc */
static uint32_t s_cover_id = 0;         /* bumped per track: older decodes are dropped */

/* ---------- helpers ---------- */

static bool has_non_ascii(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

static void set_text_font(lv_obj_t *lbl, const char *text, const lv_font_t *ascii_font) {
    lv_obj_set_style_text_font(lbl, has_non_ascii(text) ? ui_font_get() : ascii_font, 0);
}

static void fmt_time(char *buf, size_t n, float s) {
    const unsigned t = s > 0 ? (unsigned)s : 0;
    if (t >= 3600) {
        snprintf(buf, n, "%u:%02u:%02u", t / 3600, t / 60 % 60, t % 60);
    } else {
        snprintf(buf, n, "%u:%02u", t / 60, t % 60);
    }
}

/** List label: path below /music (or the file name), without ".mp3". */
static std::string track_label(const std::string &path) {
    const std::string base = std::string(k_library_dir) + "/";
    std::string name = path.compare(0, base.size(), base) == 0 ? path.substr(base.size()) : path.substr(path.rfind('/') + 1);
    if (audio_is_mp3(name.c_str())) {
        name.resize(name.size() - 4);
    }
    return name;
}

static void scan_dir(const std::string &dir, int depth, std::vector<std::string> *out) {
    DIR *d = opendir(sd_path(dir.c_str()).c_str());
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr && out->size() < k_max_tracks) {
        if (de->d_name[0] == '.') {
            continue; /* hidden, macOS "._" files */
        }
        const std::string path = dir + "/" + de->d_name;
        if (de->d_type == DT_DIR) {
            if (depth < k_max_depth) {
                scan_dir(path, depth + 1, out);
            }
        } else if (audio_is_mp3(de->d_name)) {
            out->push_back(path);
        }
    }
    closedir(d);
}

/* ---------- cover ---------- */

/** Back to the placeholder and free the picture. */
static void cover_clear(void) {
    if (s_cover_img) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_cover_img, nullptr);
        lv_obj_clear_flag(s_cover_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_cover_buf) {
        lv_img_cache_invalidate_src(&s_cover_dsc);
        heap_caps_free(s_cover_buf);
        s_cover_buf = nullptr;
    }
}

/** New track: drop the old picture and start decoding the new one (if the file has one). */
static void cover_load(const AudioStatus &st) {
    cover_clear();
    s_cover_id++;
    if (st.cover_len > 0) {
        cover_art_request(st.path.c_str(), st.cover_off, st.cover_len, k_cover, s_cover_id);
    }
}

/** Show a finished decode for the current track; discard stale ones. */
static void cover_poll(void) {
    uint32_t id;
    uint16_t *buf;
    int w, h;
    if (!cover_art_take(&id, &buf, &w, &h)) {
        return;
    }
    if (id != s_cover_id || !buf || !s_cover_img) {
        heap_caps_free(buf);
        return;
    }
    cover_clear();
    s_cover_buf = buf;
    memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
    s_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.header.w = w;
    s_cover_dsc.header.h = h;
    s_cover_dsc.data_size = (uint32_t)w * h * 2;
    s_cover_dsc.data = (const uint8_t *)buf;
    lv_img_set_src(s_cover_img, &s_cover_dsc);
    lv_obj_center(s_cover_img);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cover_icon, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- list ---------- */

static void row_cb(lv_event_t *e) {
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx < s_tracks.size()) {
        audio_play(s_tracks, idx);
    }
}

static void style_row(int i, bool current) {
    if (i < 0 || i >= (int)s_rows.size()) {
        return;
    }
    lv_obj_t *row = s_rows[i];
    lv_obj_t *icon = lv_obj_get_child(row, 0);
    lv_obj_t *name = lv_obj_get_child(row, 1);
    const lv_color_t accent = lv_palette_main(LV_PALETTE_PINK);
    lv_obj_set_style_bg_opa(row, current ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_palette_lighten(LV_PALETTE_PINK, 5), 0);
    lv_label_set_text(icon, current ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(icon, current ? accent : ui_color_text_muted(), 0);
    lv_obj_set_style_text_color(name, current ? accent : ui_color_text(), 0);
}

static void add_row(size_t idx) {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, ui_color_track(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, ui_color_track(), LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_set_width(icon, 18);

    const std::string label = track_label(s_tracks[idx]);
    lv_obj_t *name = lv_label_create(row);
    set_text_font(name, label.c_str(), &lv_font_montserrat_16);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, label.c_str());

    s_rows.push_back(row);
    style_row((int)idx, false);
}

static void show_tracks(const std::vector<std::string> &tracks) {
    s_tracks = tracks;
    s_rows.clear();
    s_highlight = -1;
    s_shown_path.clear(); /* re-highlight on the next tick */
    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_tracks.size(); i++) {
        add_row(i);
    }
    if (s_tracks.empty()) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(empty, ui_color_text_muted(), 0);
        lv_obj_set_style_pad_all(empty, 16, 0);
        lv_label_set_text(empty, sd_card_mounted() ? "No MP3 files found.\n\nCopy music to the /music folder on the SD card."
                                                   : "No SD card");
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

static void load_library(void) {
    const uint32_t t0 = now_ms();
    std::vector<std::string> tracks;
    scan_dir(k_library_dir, 1, &tracks);
    std::sort(tracks.begin(), tracks.end(),
              [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    show_tracks(tracks);
    serial_log("[music] %s: %u track(s) in %lu ms", k_library_dir, (unsigned)tracks.size(), (unsigned long)(now_ms() - t0));
}

/* ---------- now playing ---------- */

static void refresh(void) {
    AudioStatus st;
    audio_get_status(&st);

    if (st.path != s_shown_path || st.title != s_shown_title) {
        if (st.path != s_shown_path) {
            cover_load(st);
        }
        s_shown_path = st.path;
        s_shown_title = st.title;
        const char *title = st.path.empty() ? "Not playing" : st.title.c_str();
        set_text_font(s_title, title, &lv_font_montserrat_20);
        lv_label_set_text(s_title, title); /* only on change: restarts the scroll */
        set_text_font(s_artist, st.artist.c_str(), &lv_font_montserrat_16);
        lv_label_set_text(s_artist, st.artist.c_str());

        int found = -1;
        for (size_t i = 0; i < s_tracks.size() && !st.path.empty(); i++) {
            if (s_tracks[i] == st.path) {
                found = (int)i;
                break;
            }
        }
        if (found != s_highlight) {
            style_row(s_highlight, false);
            style_row(found, true);
            s_highlight = found;
            if (found >= 0) {
                lv_obj_scroll_to_view(s_rows[found], LV_ANIM_ON);
            }
        }
    }
    cover_poll();

    if (st.error && st.state == AudioState::Stopped) {
        lv_label_set_text(s_info, "Could not play this file");
    } else if (st.sample_rate) {
        lv_label_set_text_fmt(s_info, "%lu.%lu kHz  %lu kbps\nTrack %u of %u", (unsigned long)(st.sample_rate / 1000),
                              (unsigned long)(st.sample_rate % 1000 / 100), (unsigned long)st.bitrate_kbps,
                              (unsigned)(st.index + 1), (unsigned)st.count);
    } else {
        lv_label_set_text(s_info, "");
    }

    lv_label_set_text(s_play_lbl, st.state == AudioState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    char buf[16];
    if (st.dur_s > 0) {
        lv_obj_clear_state(s_seek, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_seek, LV_STATE_DISABLED);
    }
    if (!lv_obj_has_state(s_seek, LV_STATE_PRESSED)) { /* don't fight a drag */
        const int permille = st.dur_s > 0 ? (int)(st.pos_s / st.dur_s * 1000) : 0;
        lv_slider_set_value(s_seek, permille > 1000 ? 1000 : permille, LV_ANIM_OFF);
        fmt_time(buf, sizeof(buf), st.pos_s);
        lv_label_set_text(s_pos_lbl, buf);
    }
    if (st.dur_s > 0) {
        fmt_time(buf, sizeof(buf), st.dur_s);
        lv_label_set_text(s_dur_lbl, buf);
    } else {
        lv_label_set_text(s_dur_lbl, "--:--");
    }
}

static void timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (lv_scr_act() == s_scr) {
        refresh();
    }
}

/* ---------- controls ---------- */

static void play_cb(lv_event_t *e) {
    LV_UNUSED(e);
    AudioStatus st;
    audio_get_status(&st);
    if (st.path.empty() && st.state == AudioState::Stopped && !s_tracks.empty()) {
        audio_play(s_tracks, 0); /* nothing played yet: start the list */
    } else {
        audio_toggle_pause();
    }
}

static void prev_cb(lv_event_t *e) {
    LV_UNUSED(e);
    audio_prev();
}

static void next_cb(lv_event_t *e) {
    LV_UNUSED(e);
    audio_next();
}

static void seek_cb(lv_event_t *e) {
    const int v = lv_slider_get_value(s_seek);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        audio_seek(v / 1000.0f);
        return;
    }
    /* Dragging: preview the time. */
    AudioStatus st;
    audio_get_status(&st);
    char buf[16];
    fmt_time(buf, sizeof(buf), st.dur_s * v / 1000.0f);
    lv_label_set_text(s_pos_lbl, buf);
}

static void volume_cb(lv_event_t *e) {
    const int v = lv_slider_get_value(s_vol);
    const bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", v);
    settings_set_volume(v, released);
    if (released) {
        serial_log("[music] volume %d%%", v);
    }
}

/*
 * The volume row sits at the bottom of the screen, and presses there tend to land just below it
 * (the log showed them at y = 308, under the row). Accept any press from the top of the row down to
 * the screen edge, across the slider's width. lv_obj_hit_test() checks the extended click area
 * first and the HIT_TEST event can only narrow it, so the click area is made generous and trimmed
 * here; the containers above the slider are OVERFLOW_VISIBLE so the search reaches it at all.
 */
static const lv_coord_t k_vol_ext_click = 40;

static void volume_hit_test_cb(lv_event_t *e) {
    lv_hit_test_info_t *info = (lv_hit_test_info_t *)lv_event_get_param(e);
    lv_area_t a, row;
    lv_obj_get_coords(s_vol, &a);
    lv_obj_get_coords(lv_obj_get_parent(s_vol), &row);
    a.x1 -= 12;
    a.x2 += 12;
    a.y1 = row.y1;
    a.y2 = LV_VER_RES - 1;
    info->res = _lv_area_is_point_on(&a, info->point, 0);
}

static void refresh_cb(lv_event_t *e) {
    LV_UNUSED(e);
    load_library();
}

static lv_obj_t *add_ctrl_btn(lv_obj_t *parent, const char *symbol, lv_coord_t size, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, size >= 52 ? &lv_font_montserrat_24 : &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *add_plain_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *add_small_label(lv_obj_t *parent) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, ui_color_text_muted(), 0);
    return lbl;
}

/**
 * Slider in a row tall enough to touch comfortably. A touch only reaches a child inside its
 * parent's area, so the row - not the slider's own extended click area - sets the target height.
 */
static lv_obj_t *add_slider_row(lv_obj_t *parent, lv_obj_t **slider) {
    lv_obj_t *row = add_plain_row(parent);
    lv_obj_set_height(row, 36);
    lv_obj_set_style_pad_column(row, 10, 0);
    *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(*slider, 1);
    lv_obj_set_ext_click_area(*slider, 12);
    return row;
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

    lv_obj_t *bar = status_bar_create(s_scr, "Music", true);
    (void)status_bar_add_action(bar, LV_SYMBOL_REFRESH, refresh_cb, nullptr);

    lv_obj_t *body = add_plain_row(s_scr);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, 8, 0);

    /* Now playing: rows spread over the card's height (about 256 px - 20 padding). */
    lv_obj_t *card = lv_obj_create(body);
    ui_style_card(card);
    lv_obj_set_size(card, 240, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = add_plain_row(card);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(top, 10, 0);

    lv_obj_t *cover = lv_obj_create(top);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, k_cover, k_cover);
    lv_obj_set_style_radius(cover, 8, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cover, lv_palette_lighten(LV_PALETTE_PINK, 4), 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    s_cover_icon = lv_label_create(cover);
    lv_obj_set_style_text_font(s_cover_icon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(s_cover_icon, lv_palette_main(LV_PALETTE_PINK), 0);
    lv_label_set_text(s_cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(s_cover_icon);
    s_cover_img = lv_img_create(cover);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *text = lv_obj_create(top);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, k_cover);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 2, 0);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(text);
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_title, ui_color_text(), 0);

    s_artist = lv_label_create(text);
    lv_obj_set_width(s_artist, LV_PCT(100));
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_artist, ui_color_text_muted(), 0);

    s_info = add_small_label(text);
    lv_obj_set_width(s_info, LV_PCT(100));
    lv_obj_set_style_pad_top(s_info, 4, 0);

    /* 0:00 [====seek====] 3:45 */
    lv_obj_t *seek_row = add_slider_row(card, &s_seek);
    lv_slider_set_range(s_seek, 0, 1000);
    lv_obj_add_event_cb(s_seek, seek_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_seek, seek_cb, LV_EVENT_RELEASED, nullptr);
    s_pos_lbl = add_small_label(seek_row);
    lv_obj_move_to_index(s_pos_lbl, 0);
    s_dur_lbl = add_small_label(seek_row);

    lv_obj_t *ctrl_row = add_plain_row(card);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    (void)add_ctrl_btn(ctrl_row, LV_SYMBOL_PREV, 44, prev_cb);
    lv_obj_t *play = add_ctrl_btn(ctrl_row, LV_SYMBOL_PLAY, 52, play_cb);
    lv_obj_set_style_bg_color(play, lv_palette_main(LV_PALETTE_PINK), 0);
    s_play_lbl = lv_obj_get_child(play, 0);
    (void)add_ctrl_btn(ctrl_row, LV_SYMBOL_NEXT, 44, next_cb);

    /* (vol) [====volume====] 50% */
    lv_obj_t *vol_row = add_slider_row(card, &s_vol);
    lv_obj_set_style_pad_hor(vol_row, 4, 0);
    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_obj_set_style_text_color(vol_icon, ui_color_text_muted(), 0);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_move_to_index(vol_icon, 0);
    s_vol_lbl = add_small_label(vol_row);
    lv_obj_set_width(s_vol_lbl, 32);
    lv_obj_set_style_text_align(s_vol_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, settings_get().volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_lbl, "%d%%", settings_get().volume);
    lv_obj_add_event_cb(s_vol, volume_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_vol, volume_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_set_ext_click_area(s_vol, k_vol_ext_click);
    lv_obj_add_flag(s_vol, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(s_vol, volume_hit_test_cb, LV_EVENT_HIT_TEST, nullptr);
    lv_obj_add_flag(vol_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* Track list */
    s_list = lv_obj_create(body);
    ui_style_card(s_list);
    lv_obj_set_height(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_timer = lv_timer_create(timer_cb, 250, nullptr);
}

void music_ui_open(void) {
    build_screen();
    const std::vector<std::string> playlist = audio_playlist();
    if (!playlist.empty()) {
        show_tracks(playlist);
    } else if (s_tracks.empty()) {
        load_library();
    }
    lv_scr_load(s_scr);
    refresh();
}

void music_ui_release(void) {
    if (!s_scr || lv_scr_act() == s_scr) {
        return;
    }
    lv_timer_del(s_timer);
    cover_clear();
    s_cover_id++; /* a decode still running is dropped when it arrives */
    lv_obj_del(s_scr);
    s_scr = s_list = s_cover_img = s_cover_icon = s_title = s_artist = s_info = nullptr;
    s_seek = s_pos_lbl = s_dur_lbl = s_play_lbl = s_vol = s_vol_lbl = nullptr;
    s_timer = nullptr;
    s_tracks.clear();
    s_tracks.shrink_to_fit();
    s_rows.clear();
    s_rows.shrink_to_fit();
    s_shown_path.clear();
    s_shown_title.clear();
    s_highlight = -1;
}
