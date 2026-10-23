#include "music_ui.h"

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lvgl.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <string.h>
#include <strings.h>
#include <vector>

#include "app_settings.h"
#include "audio_player.h"
#include "cover_art.h"
#include "lyrics.h"
#include "platform_time.h"
#include "sd_card.h"
#include "serial_log.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"
#include "volume_ui.h"

/*
 * Music player: now-playing card (cover, title, artist, seek bar, prev / play / next, volume) next
 * to the track list. Playback itself runs in audio_player.cpp and continues after leaving this
 * screen; the screen only mirrors its status (timer) and sends commands. Cover art is decoded in
 * the background (cover_art.cpp) and shown when ready.
 *
 * The right side switches between Lyrics and Tracks. Lyrics come from lyrics.cpp (a .lrc next to
 * the song, or LRCLIB); synced ones highlight the current line and keep it centred (paused for a
 * few seconds after a manual scroll), and tapping a line seeks there.
 *
 * The list is the player's playlist, or - before anything was played, or after Refresh - every
 * MP3 under /music (subfolders included). Tapping a track plays the list from there; with shuffle
 * on (top bar, saved in settings.ini) the rest follows in random order, and Play with nothing
 * played yet starts at a random track.
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
static lv_obj_t *s_vol_row = nullptr;
static lv_timer_t *s_timer = nullptr;

static std::vector<std::string> s_tracks; /* card paths shown in the list */
static std::vector<lv_obj_t *> s_rows;
static std::string s_shown_path;  /* track the card / highlight currently show */
static std::string s_shown_title;
static int s_highlight = -1;

static uint32_t s_shown_seq = 0;  /* AudioStatus::seq the cover and lyrics belong to */

static lv_img_dsc_t s_cover_dsc;
static uint16_t *s_cover_buf = nullptr; /* pixels behind s_cover_dsc */
static uint32_t s_cover_id = 0;         /* bumped per track: older decodes are dropped */

static lv_obj_t *s_view_bm = nullptr;         /* Lyrics | Tracks */
static lv_obj_t *s_lyr = nullptr;             /* lyrics card (scrolls) */
static int s_view = -1;                       /* 0 lyrics, 1 tracks; kept while the screen is released */
static LyricsResult *s_lyr_res = nullptr;     /* lyrics on screen */
static std::vector<lv_obj_t *> s_lyr_labels;  /* per line; nullptr for a pause (empty line) */
static int s_lyr_cur = -1;                    /* highlighted line */
static int s_lyr_word = -1;                   /* words of it shown as sung (word-timed lines) */
static uint32_t s_lyr_id = 0;                 /* bumped per request: older results are dropped */
static uint32_t s_lyr_user_ms = 0;            /* last manual scroll (0: none pending) */

/* ---------- helpers ---------- */

static bool has_non_ascii(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

/**
 * lv_label_set_text() invalidates even when the text is the same, and every redraw on this panel is
 * a full frame (~80 ms): status labels updated by the timer only change when their text does.
 */
static void set_text_if_changed(lv_obj_t *lbl, const char *text) {
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
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

/* ---------- lyrics ---------- */

static const int32_t k_lyr_lead_ms = 200;     /* highlight a line just before it is sung */
static const uint32_t k_lyr_manual_ms = 4000; /* after a manual scroll, no auto-scroll for this long */

static void lyrics_clear(void) {
    if (s_lyr) {
        lv_obj_clean(s_lyr);
    }
    s_lyr_labels.clear();
    s_lyr_cur = -1;
    s_lyr_word = -1;
    delete s_lyr_res;
    s_lyr_res = nullptr;
}

/** Scroll so line i is in the middle of the card. */
static void lyrics_scroll_to(int i, lv_anim_enable_t anim) {
    if (i < 0 || i >= (int)s_lyr_labels.size() || !s_lyr_labels[i]) {
        return;
    }
    lv_obj_t *l = s_lyr_labels[i];
    lv_obj_update_layout(s_lyr);
    const lv_coord_t y = lv_obj_get_style_pad_top(s_lyr, 0) + lv_obj_get_y(l) + lv_obj_get_height(l) / 2 -
                         lv_obj_get_height(s_lyr) / 2;
    lv_obj_scroll_to_y(s_lyr, y, anim);
}

static lv_obj_t *lyrics_label(int i) {
    return i >= 0 && i < (int)s_lyr_labels.size() ? s_lyr_labels[i] : nullptr;
}

/** Back to a plain, not-current line. */
static void lyrics_reset_line(int i) {
    lv_obj_t *l = lyrics_label(i);
    if (l) {
        lv_label_set_recolor(l, false);
        lv_label_set_text(l, s_lyr_res->line(i));
        lv_obj_set_style_text_color(l, ui_color_text_muted(), 0);
    }
}

/**
 * The current line with its first k words sung: those in the accent colour, the rest in the text
 * colour (LVGL recolor markup "#RRGGBB text#"). Without word timing - or with a '#' in the text,
 * which the markup can't escape inside a coloured run - the whole line is highlighted.
 */
static void lyrics_render_current(int i, int k) {
    lv_obj_t *l = lyrics_label(i);
    if (!l) {
        return;
    }
    const LyricLine &ln = s_lyr_res->lines[i];
    const char *text = s_lyr_res->line(i);
    if (ln.nwords == 0 || strchr(text, '#')) {
        lv_obj_set_style_text_color(l, lv_palette_main(LV_PALETTE_PINK), 0);
        return;
    }
    const size_t len = strlen(text);
    const size_t split = k <= 0 ? 0 : k >= (int)ln.nwords ? len : s_lyr_res->words[ln.word0 + k].pos;
    std::string m;
    if (split > 0) {
        m = "#E91E63 "; /* LV_PALETTE_PINK */
        m.append(text, split);
        m += '#';
    }
    m.append(text + split, len - split);
    lv_obj_set_style_text_color(l, ui_color_text(), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, m.c_str());
}

static void lyrics_load(const AudioStatus &st, bool retry);

static void lyrics_retry_cb(lv_event_t *e) {
    LV_UNUSED(e);
    AudioStatus st;
    audio_get_status(&st);
    lyrics_load(st, true);
}

/** A message instead of lyrics, optionally with "Search again". */
static void lyrics_note(const char *text, bool retry) {
    lyrics_clear();
    lv_obj_t *lbl = lv_label_create(s_lyr);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, ui_color_text_muted(), 0);
    lv_label_set_text(lbl, text);
    if (retry) {
        lv_obj_t *btn = lv_btn_create(s_lyr);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, lyrics_retry_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Search again");
    }
    lv_obj_scroll_to_y(s_lyr, 0, LV_ANIM_OFF);
}

/** Tap on a synced line: play from there. */
static void lyrics_line_cb(lv_event_t *e) {
    const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    AudioStatus st;
    audio_get_status(&st);
    if (s_lyr_res && i < s_lyr_res->lines.size() && st.dur_s > 0) {
        audio_seek(s_lyr_res->lines[i].ms / 1000.0f / st.dur_s);
        s_lyr_user_ms = 0; /* follow the song again right away */
    }
}

/** Show a result (takes ownership). */
static void lyrics_show(LyricsResult *r) {
    if (r->kind == LyricsKind::None) {
        lyrics_note(r->note.c_str(), r->can_retry);
        delete r;
        return;
    }
    lyrics_clear();
    s_lyr_res = r;
    const bool synced = r->kind == LyricsKind::Synced;
    bool cjk = false; /* one font for the whole song */
    for (size_t i = 0; i < r->lines.size() && !cjk; i++) {
        cjk = has_non_ascii(r->line(i));
    }
    for (size_t i = 0; i < r->lines.size(); i++) {
        const char *text = r->line(i);
        if (!text[0]) {
            s_lyr_labels.push_back(nullptr); /* a pause: nothing highlighted */
            continue;
        }
        lv_obj_t *lbl = lv_label_create(s_lyr);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, cjk ? ui_font_get() : &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, synced ? ui_color_text_muted() : ui_color_text(), 0);
        lv_label_set_text(lbl, text);
        if (synced) {
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(lbl, lyrics_line_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        }
        s_lyr_labels.push_back(lbl);
    }
    s_lyr_user_ms = 0;
    lv_obj_scroll_to_y(s_lyr, 0, LV_ANIM_OFF);
}

/** New track (or Search again): ask for its lyrics. */
static void lyrics_load(const AudioStatus &st, bool retry) {
    s_lyr_id++;
    if (st.stream) {
        lyrics_note("No lyrics for internet radio", false);
        return;
    }
    if (st.path.empty() || st.error) {
        lyrics_note("Lyrics appear here when a song plays", false);
        return;
    }
    lyrics_note("Looking for lyrics...", false);
    lyrics_request(st.path, st.title, st.artist, st.dur_s, s_lyr_id, retry);
}

static void lyrics_poll(void) {
    LyricsResult *r = lyrics_take();
    if (!r) {
        return;
    }
    if (r->id != s_lyr_id) {
        delete r; /* for a track that is no longer current */
        return;
    }
    lyrics_show(r);
}

/**
 * Highlight the line being sung - word by word when the lyrics have word timing - and keep it
 * centred unless the user is scrolling.
 */
static void lyrics_tick(const AudioStatus &st) {
    if (!s_lyr_res || s_lyr_res->kind != LyricsKind::Synced) {
        return;
    }
    const int32_t pos_ms = (int32_t)(st.pos_s * 1000) + k_lyr_lead_ms;
    int cur = -1;
    for (size_t i = 0; i < s_lyr_res->lines.size() && s_lyr_res->lines[i].ms <= pos_ms; i++) {
        cur = (int)i;
    }
    const bool manual = s_lyr_user_ms != 0 && now_ms() - s_lyr_user_ms < k_lyr_manual_ms;
    if (!manual && s_lyr_user_ms != 0) {
        s_lyr_user_ms = 0; /* manual scroll timed out: back to the current line */
        lyrics_scroll_to(s_lyr_cur, LV_ANIM_ON);
    }
    if (cur != s_lyr_cur) {
        lyrics_reset_line(s_lyr_cur);
        s_lyr_cur = cur;
        s_lyr_word = -1;
        if (!manual && s_view == 0) {
            lyrics_scroll_to(cur, LV_ANIM_ON);
        }
    }
    if (cur < 0) {
        return;
    }
    const LyricLine &ln = s_lyr_res->lines[cur];
    int k = 0;
    for (uint32_t w = 0; w < ln.nwords && s_lyr_res->words[ln.word0 + w].ms <= pos_ms; w++) {
        k++;
    }
    if (k != s_lyr_word) {
        s_lyr_word = k;
        lyrics_render_current(cur, k);
    }
}

/** Scrolls started by a finger pause the auto-scroll (programmatic ones have no active input). */
static void lyrics_scroll_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN && lv_indev_get_act()) {
        s_lyr_user_ms = now_ms() | 1;
    } else if (code == LV_EVENT_SCROLL_END && s_lyr_user_ms != 0) {
        s_lyr_user_ms = now_ms() | 1; /* count from the end of the fling */
    }
}

static void show_view(int v) {
    s_view = v;
    lv_obj_add_flag(v == 0 ? s_list : s_lyr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? s_lyr : s_list, LV_OBJ_FLAG_HIDDEN);
    lv_btnmatrix_set_btn_ctrl(s_view_bm, v, LV_BTNMATRIX_CTRL_CHECKED);
    if (v == 0) {
        s_lyr_user_ms = 0;
        lyrics_scroll_to(s_lyr_cur, LV_ANIM_OFF);
    }
}

static void view_cb(lv_event_t *e) {
    LV_UNUSED(e);
    const uint16_t sel = lv_btnmatrix_get_selected_btn(s_view_bm);
    if (sel <= 1) {
        show_view(sel);
    }
}

/* ---------- now playing ---------- */

static void refresh(void) {
    AudioStatus st;
    audio_get_status(&st);

    if (st.seq != s_shown_seq) { /* a new track's details are complete */
        s_shown_seq = st.seq;
        cover_load(st);
        lyrics_load(st, false);
    }
    lyrics_poll();
    lyrics_tick(st);

    if (st.path != s_shown_path || st.title != s_shown_title) {
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

    char buf[64];
    if (st.error && st.state == AudioState::Stopped) {
        set_text_if_changed(s_info, "Could not play this file");
    } else if (st.sample_rate) {
        snprintf(buf, sizeof(buf), "%lu.%lu kHz  %lu kbps\nTrack %u of %u", (unsigned long)(st.sample_rate / 1000),
                 (unsigned long)(st.sample_rate % 1000 / 100), (unsigned long)st.bitrate_kbps, (unsigned)(st.index + 1),
                 (unsigned)st.count);
        set_text_if_changed(s_info, buf);
    } else {
        set_text_if_changed(s_info, "");
    }

    set_text_if_changed(s_play_lbl, st.state == AudioState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (st.dur_s > 0) {
        lv_obj_clear_state(s_seek, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_seek, LV_STATE_DISABLED);
    }
    if (!lv_obj_has_state(s_seek, LV_STATE_PRESSED)) { /* don't fight a drag */
        int permille = st.dur_s > 0 ? (int)(st.pos_s / st.dur_s * 1000) : 0;
        permille = permille > 1000 ? 1000 : permille;
        /* Steps of 0.5 %: the knob moves less than a pixel per permille, and every change is a redraw. */
        if (abs(permille - lv_slider_get_value(s_seek)) >= 5 || permille == 0) {
            lv_slider_set_value(s_seek, permille, LV_ANIM_OFF);
        }
        fmt_time(buf, sizeof(buf), st.pos_s);
        set_text_if_changed(s_pos_lbl, buf);
    }
    if (st.dur_s > 0) {
        fmt_time(buf, sizeof(buf), st.dur_s);
        set_text_if_changed(s_dur_lbl, buf);
    } else {
        set_text_if_changed(s_dur_lbl, "--:--");
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
        /* Nothing played yet: start the list - at a random track when shuffling. */
        audio_play(s_tracks, settings_get().shuffle ? esp_random() % s_tracks.size() : 0);
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

static void refresh_cb(lv_event_t *e) {
    LV_UNUSED(e);
    load_library();
}

static void style_shuffle_btn(lv_obj_t *btn, bool on) {
    lv_obj_set_style_bg_color(btn, on ? lv_palette_main(LV_PALETTE_PINK) : ui_color_track(), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : ui_color_text_muted(), 0);
}

static void shuffle_cb(lv_event_t *e) {
    const bool on = !settings_get().shuffle;
    settings_set_shuffle(on);
    style_shuffle_btn(lv_event_get_target(e), on);
    serial_log("[music] shuffle %s", on ? "on" : "off");
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
    lv_obj_t *shuffle = status_bar_add_action(bar, LV_SYMBOL_SHUFFLE, shuffle_cb, nullptr);
    style_shuffle_btn(shuffle, settings_get().shuffle);
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

    s_vol_row = volume_ui_create(card);

    /* Right side: [Lyrics | Tracks] over the lyrics card or the track list */
    lv_obj_t *right = lv_obj_create(body);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    static const char *k_view_map[] = {"Lyrics", "Tracks", ""};
    s_view_bm = lv_btnmatrix_create(right);
    lv_btnmatrix_set_map(s_view_bm, k_view_map);
    lv_btnmatrix_set_btn_ctrl_all(s_view_bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(s_view_bm, true);
    lv_obj_set_size(s_view_bm, LV_PCT(100), 32);
    ui_style_segmented(s_view_bm);
    lv_obj_add_event_cb(s_view_bm, view_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lyr = lv_obj_create(right);
    ui_style_card(s_lyr);
    lv_obj_set_width(s_lyr, LV_PCT(100));
    lv_obj_set_flex_grow(s_lyr, 1);
    lv_obj_set_style_pad_hor(s_lyr, 10, 0);
    lv_obj_set_style_pad_ver(s_lyr, 80, 0); /* lets the first and last lines reach the middle */
    lv_obj_set_style_pad_row(s_lyr, 10, 0);
    lv_obj_set_flex_flow(s_lyr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lyr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_lyr, LV_DIR_VER);
    lv_obj_add_event_cb(s_lyr, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(s_lyr, lyrics_scroll_cb, LV_EVENT_SCROLL_END, nullptr);
    lyrics_note("Lyrics appear here when a song plays", false);

    s_list = lv_obj_create(right);
    ui_style_card(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_timer = lv_timer_create(timer_cb, 100, nullptr); /* word-timed lyrics: words are ~200 ms apart */
}

void music_ui_open(void) {
    build_screen();
    const std::vector<std::string> playlist = audio_playlist();
    if (!playlist.empty()) {
        show_tracks(playlist);
    } else if (s_tracks.empty()) {
        load_library();
    }
    if (s_view < 0) {
        s_view = playlist.empty() ? 1 : 0; /* first time: pick songs, or watch the lyrics */
    }
    show_view(s_view);
    volume_ui_sync(s_vol_row); /* may have changed in Radio */
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
    lyrics_clear();
    s_lyr_id++;   /* likewise a lookup */
    s_shown_seq = 0;
    lv_obj_del(s_scr);
    s_view_bm = s_lyr = nullptr;
    s_scr = s_list = s_cover_img = s_cover_icon = s_title = s_artist = s_info = nullptr;
    s_seek = s_pos_lbl = s_dur_lbl = s_play_lbl = s_vol_row = nullptr;
    s_timer = nullptr;
    s_tracks.clear();
    s_tracks.shrink_to_fit();
    s_rows.clear();
    s_rows.shrink_to_fit();
    s_shown_path.clear();
    s_shown_title.clear();
    s_highlight = -1;
}
