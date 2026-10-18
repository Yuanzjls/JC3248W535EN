#include "files_ui.h"

#include <Arduino.h>
#include "sd_card.h"
#include <lvgl.h>
#include <algorithm>
#include <strings.h>
#include <time.h>
#include <vector>

#include "media_ui.h"
#include "serial_log.h"
#include "platform_time.h"
#include "status_bar.h"
#include "ui_font.h"
#include "ui_theme.h"

/*
 * Read-only SD card browser: folders first, then files, sorted by name. Text files open in a
 * viewer (first 16 KB); anything else shows an info card. SD access runs on the LVGL task; FATFS
 * has its own locking, so it is safe alongside the other tasks that touch the card.
 */

static const size_t k_max_entries = 200;      /* big folders: list stays responsive */
static const size_t k_max_text = 16 * 1024;   /* viewer limit */
static const size_t k_text_sniff = 512;       /* bytes checked to decide "is this text?" */

struct Entry {
    String name;
    bool dir;
    uint32_t size;
    time_t mtime;
};

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_path_lbl = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_footer = nullptr;
static lv_obj_t *s_up_btn = nullptr;

static lv_obj_t *s_view_scr = nullptr;
static lv_obj_t *s_view_bar = nullptr;
static lv_obj_t *s_view_body = nullptr;

static String s_cwd = "/";
static std::vector<Entry> s_entries;

/* ---------- helpers ---------- */

static String join_path(const String &dir, const String &name) {
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

static String parent_path(const String &path) {
    const int slash = path.lastIndexOf('/');
    return slash <= 0 ? String("/") : path.substring(0, slash);
}

static void fmt_size(char *buf, size_t n, uint64_t bytes) {
    if (bytes < 1024) {
        snprintf(buf, n, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, n, "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(buf, n, "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, n, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
}

static bool ends_with_ci(const String &s, const char *suffix) {
    const size_t n = strlen(suffix);
    return s.length() >= n && strcasecmp(s.c_str() + s.length() - n, suffix) == 0;
}

static bool is_text_ext(const String &name) {
    static const char *const k_ext[] = {".txt", ".ini", ".json", ".csv", ".log", ".md", ".cfg", ".conf", ".xml", ".htm",
                                        ".html", ".h", ".c", ".cpp", ".py", ".js", ".css", ".yaml", ".yml"};
    for (const char *e : k_ext) {
        if (ends_with_ci(name, e)) {
            return true;
        }
    }
    return false;
}

/** Icon and colour by file type. */
static const char *icon_for(const Entry &e, lv_color_t *color) {
    if (e.dir) {
        *color = lv_palette_main(LV_PALETTE_AMBER);
        return LV_SYMBOL_DIRECTORY;
    }
    static const char *const k_img[] = {".jpg", ".jpeg", ".png", ".bmp", ".gif"};
    static const char *const k_audio[] = {".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg", ".opus"};
    for (const char *x : k_img) {
        if (ends_with_ci(e.name, x)) {
            *color = lv_palette_main(LV_PALETTE_GREEN);
            return LV_SYMBOL_IMAGE;
        }
    }
    if (media_is_video(e.name)) {
        *color = lv_palette_main(LV_PALETTE_RED);
        return LV_SYMBOL_VIDEO;
    }
    for (const char *x : k_audio) {
        if (ends_with_ci(e.name, x)) {
            *color = lv_palette_main(LV_PALETTE_PURPLE);
            return LV_SYMBOL_AUDIO;
        }
    }
    *color = is_text_ext(e.name) ? lv_palette_main(LV_PALETTE_BLUE) : ui_color_text_muted();
    return LV_SYMBOL_FILE;
}

static bool has_non_ascii(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

/* ---------- viewer ---------- */

static void view_back_cb(lv_event_t *e) {
    LV_UNUSED(e);
    lv_scr_load(s_scr);
}

static void build_viewer(void) {
    if (s_view_scr) {
        return;
    }
    s_view_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_opa(s_view_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_view_scr, ui_color_bg(), 0);
    lv_obj_set_style_pad_all(s_view_scr, 8, 0);
    lv_obj_set_style_pad_row(s_view_scr, 6, 0);
    lv_obj_set_flex_flow(s_view_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_view_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_view_bar = status_bar_create(s_view_scr, "", true);
    (void)status_bar_add_action(s_view_bar, LV_SYMBOL_LEFT, view_back_cb, nullptr);

    lv_obj_t *card = lv_obj_create(s_view_scr);
    ui_style_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);

    s_view_body = lv_label_create(card);
    lv_obj_set_width(s_view_body, LV_PCT(100));
    lv_label_set_long_mode(s_view_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_view_body, ui_color_text(), 0);
}

static void open_file(const Entry &e) {
    build_viewer();
    status_bar_set_title(s_view_bar, e.name.c_str());
    lv_obj_scroll_to_y(lv_obj_get_parent(s_view_body), 0, LV_ANIM_OFF);

    const String path = join_path(s_cwd, e.name);
    File f = sd_fs().open(path, FILE_READ);
    if (!f) {
        lv_obj_set_style_text_font(s_view_body, &lv_font_montserrat_16, 0);
        lv_label_set_text(s_view_body, "Could not open this file.");
        lv_scr_load(s_view_scr);
        return;
    }

    /* Read up to the viewer limit (PSRAM), then decide whether it's text. */
    const size_t want = e.size < k_max_text ? e.size : k_max_text;
    char *buf = (char *)heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t got = 0;
    if (buf) {
        got = sd_read(f, buf, want);
        buf[got] = '\0';
    }
    f.close();

    bool text = buf != nullptr;
    const size_t sniff = got < k_text_sniff ? got : k_text_sniff;
    for (size_t i = 0; text && i < sniff; i++) {
        const unsigned char c = (unsigned char)buf[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            text = false;
        }
    }

    char size[16];
    fmt_size(size, sizeof(size), e.size);
    if (text) {
        /* Montserrat is compact for ASCII; the SD UI font is needed for CJK and other UTF-8. */
        lv_obj_set_style_text_font(s_view_body, has_non_ascii(buf) ? ui_font_get() : &lv_font_montserrat_14, 0);
        if (got == 0) {
            lv_label_set_text(s_view_body, "(empty file)");
        } else if (e.size > k_max_text) {
            String s = String(buf) + "\n\n... (showing first 16 KB of " + size + ")";
            lv_label_set_text(s_view_body, s.c_str());
        } else {
            lv_label_set_text(s_view_body, buf);
        }
    } else {
        char when[32] = "unknown";
        if (e.mtime > 0) {
            struct tm ti;
            localtime_r(&e.mtime, &ti);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &ti);
        }
        lv_obj_set_style_text_font(s_view_body, &lv_font_montserrat_16, 0);
        lv_label_set_text_fmt(s_view_body, "Size  %s\nModified  %s\nPath  %s\n\nBinary file - no preview.", size, when,
                              path.c_str());
    }
    if (buf) {
        heap_caps_free(buf);
    }
    lv_scr_load(s_view_scr);
}

/* ---------- list ---------- */

static void show_dir(const String &path);

static void row_cb(lv_event_t *e) {
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_entries.size()) {
        return;
    }
    const Entry entry = s_entries[idx]; /* copy: show_dir() rebuilds s_entries */
    if (entry.dir) {
        show_dir(join_path(s_cwd, entry.name));
    } else if (media_is_video(entry.name)) {
        media_ui_open_video(join_path(s_cwd, entry.name));
    } else if (media_is_image(entry.name)) {
        /* Pass the folder's images so the viewer can swipe through them. */
        std::vector<String> images;
        size_t at = 0;
        for (const Entry &x : s_entries) {
            if (!x.dir && media_is_image(x.name)) {
                if (x.name == entry.name) {
                    at = images.size();
                }
                images.push_back(join_path(s_cwd, x.name));
            }
        }
        media_ui_open_images(images, at);
    } else {
        open_file(entry);
    }
}

static void up_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_cwd != "/") {
        show_dir(parent_path(s_cwd));
    }
}

static void add_row(size_t idx) {
    const Entry &e = s_entries[idx];
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
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

    lv_color_t color;
    const char *sym = icon_for(e, &color);
    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, color, 0);
    lv_obj_set_width(icon, 24);
    lv_label_set_text(icon, sym);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, has_non_ascii(e.name.c_str()) ? ui_font_get() : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name, ui_color_text(), 0);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, e.name.c_str());

    lv_obj_t *meta = lv_label_create(row);
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(meta, ui_color_text_muted(), 0);
    if (e.dir) {
        lv_label_set_text(meta, LV_SYMBOL_RIGHT);
    } else {
        char size[16];
        fmt_size(size, sizeof(size), e.size);
        lv_label_set_text(meta, size);
    }
}

static void update_footer(void) {
    uint64_t total_b = 0, used_b = 0;
    if (!sd_card_mounted() || !sd_card_usage(&total_b, &used_b)) {
        lv_label_set_text(s_footer, "No SD card");
        return;
    }
    char used[16], total[16];
    fmt_size(used, sizeof(used), used_b);
    fmt_size(total, sizeof(total), total_b);
    lv_label_set_text_fmt(s_footer, "%s card  -  %s used of %s  -  %u item(s) here", sd_card_type(), used, total,
                          (unsigned)s_entries.size());
}

static void show_dir(const String &path) {
    const uint32_t t0 = now_ms();
    s_entries.clear();
    size_t skipped = 0;

    File dir = sd_fs().open(path);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (f.name()[0] == '.') {
                /* hidden: our /.cache (video indexes), macOS "._" files, etc. */
            } else if (s_entries.size() < k_max_entries) {
                const time_t mtime = f.getLastWrite();
                s_entries.push_back({String(f.name()), f.isDirectory(), (uint32_t)f.size(), mtime});
            } else {
                skipped++;
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
        s_cwd = path;
    } else {
        serial_log("[files] cannot open %s", path.c_str());
    }

    std::sort(s_entries.begin(), s_entries.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_entries.size(); i++) {
        add_row(i);
    }
    if (s_entries.empty()) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(empty, ui_color_text_muted(), 0);
        lv_obj_set_style_pad_all(empty, 16, 0);
        lv_label_set_text(empty, sd_card_mounted() ? "Empty folder" : "No SD card");
    }
    if (skipped) {
        lv_obj_t *more = lv_label_create(s_list);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(more, ui_color_text_muted(), 0);
        lv_obj_set_style_pad_all(more, 10, 0);
        lv_label_set_text_fmt(more, "... and %u more not shown", (unsigned)skipped);
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);

    lv_obj_set_style_text_font(s_path_lbl, has_non_ascii(s_cwd.c_str()) ? ui_font_get() : &lv_font_montserrat_14, 0);
    lv_label_set_text_fmt(s_path_lbl, LV_SYMBOL_DIRECTORY "  %s", s_cwd.c_str());
    if (s_cwd == "/") {
        lv_obj_add_state(s_up_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_up_btn, LV_STATE_DISABLED);
    }
    update_footer();
    serial_log("[files] %s: %u entries in %lu ms", s_cwd.c_str(), (unsigned)s_entries.size(),
               (unsigned long)(now_ms() - t0));
}

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

    s_bar = status_bar_create(s_scr, "Files", true);
    s_up_btn = status_bar_add_action(s_bar, LV_SYMBOL_UP, up_cb, nullptr);

    s_path_lbl = lv_label_create(s_scr);
    lv_obj_set_width(s_path_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_path_lbl, ui_color_text_muted(), 0);

    s_list = lv_obj_create(s_scr);
    ui_style_card(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_footer = lv_label_create(s_scr);
    lv_obj_set_width(s_footer, LV_PCT(100));
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_footer, ui_color_text_muted(), 0);
}

void files_ui_open(void) {
    build_screen();
    show_dir(s_cwd); /* re-read: files may have changed since last time */
    lv_scr_load(s_scr);
}

void files_ui_release(void) {
    lv_obj_t *act = lv_scr_act();
    if (!s_scr || act == s_scr || act == s_view_scr) {
        return;
    }
    if (s_view_scr) {
        lv_obj_del(s_view_scr);
    }
    lv_obj_del(s_scr);
    s_scr = s_bar = s_path_lbl = s_list = s_footer = s_up_btn = nullptr;
    s_view_scr = s_view_bar = s_view_body = nullptr;
    s_entries.clear();
    s_entries.shrink_to_fit();
    /* s_cwd stays: reopening shows the same folder. */
}
