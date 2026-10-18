#include "wifi_ui.h"

#include <algorithm>
#include <string>
#include <lvgl.h>
#include <vector>

#include "esp_bsp.h"
#include "ui_font.h"
#include "clock_ui.h"
#include "home_ui.h"
#include "status_bar.h"
#include "serial_log.h"
#include "platform_time.h"
#include "wifi_net.h"
#include "wifi_store.h"

struct ScanResult {
    std::string ssid;
    int rssi;
    uint8_t enc_type;
};

static std::vector<ScanResult> s_results;
static volatile bool s_scan_requested = false;
static volatile bool s_scan_busy = false;
static bool s_waiting_scan_complete = false;

static lv_obj_t *s_wifi_scr = nullptr;
static lv_obj_t *s_status_lbl = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_scan_btn = nullptr;
static lv_obj_t *s_modal = nullptr;
static lv_obj_t *s_pwd_ta = nullptr;
static lv_obj_t *s_kb = nullptr;

/** Modal layout: resize body when the on-screen keyboard is hidden for a larger form view */
struct PasswordDlgLayout {
    lv_obj_t *body;
    lv_coord_t ver_full;
    lv_coord_t kb_band;
};
static PasswordDlgLayout s_pwd_layout;

static std::string s_pending_ssid;
static std::string s_pending_password;
static volatile bool s_pending_connect = false;
static std::string s_attempt_ssid;
static std::string s_attempt_password;
static bool s_connect_from_manual = false;
static bool s_wifi_auto_started = false;

enum class ConnState { Idle, Connecting, Connected, Failed };
static ConnState s_conn = ConnState::Idle;
static uint32_t s_connect_start_ms = 0;

static void set_status_text(const char *txt) {
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, txt);
    }
}

/** UTF-8 / Chinese SSIDs need heap strings, not tiny stack buffers */
static void set_status_string(const std::string &s) {
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, s.c_str());
    }
}

static void close_password_dialog(void) {
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = nullptr;
        s_pwd_ta = nullptr;
        s_kb = nullptr;
        s_pwd_layout.body = nullptr;
    }
}

static void password_set_keyboard_visible(bool show) {
    if (!s_kb || !s_pwd_layout.body) {
        return;
    }
    if (show) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, s_pwd_ta);
        lv_obj_set_height(s_pwd_layout.body, s_pwd_layout.ver_full - s_pwd_layout.kb_band);
    } else {
        lv_keyboard_set_textarea(s_kb, nullptr);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_pwd_layout.body, s_pwd_layout.ver_full);
    }
}

/** LVGL sends LV_EVENT_READY on the keyboard when ✔ (LV_SYMBOL_OK) is pressed */
static void kb_ready_hide_cb(lv_event_t *e) {
    LV_UNUSED(e);
    password_set_keyboard_visible(false);
}

static void pwd_ta_clicked_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!s_kb || !s_pwd_ta) {
        return;
    }
    if (lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) {
        password_set_keyboard_visible(true);
    }
}

static void connect_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (!s_pwd_ta) {
        return;
    }
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, nullptr);
    }
    /* Copy before closing dialog deletes the textarea. */
    s_pending_password = lv_textarea_get_text(s_pwd_ta);
    close_password_dialog();

    set_status_text("Connecting...");
    /* Connecting runs from loop() (wifi_ui_loop), not the LVGL task. */
    s_pending_connect = true;
}

static void cancel_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, nullptr);
    }
    close_password_dialog();
}

static void show_password_dialog(const char *ssid) {
    close_password_dialog();
    s_pending_ssid = ssid;

    lv_obj_t *scr = lv_scr_act();
    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep modal clickable so touches are not passed through to the WiFi list below. */
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_disp_t *disp = lv_disp_get_default();
    const lv_coord_t ver = lv_disp_get_ver_res(disp);
    const lv_coord_t hor = lv_disp_get_hor_res(disp);
    const lv_coord_t kb_h = LV_DPI_DEF + LV_DPI_DEF / 2;

    /*
     * Top band = form (scrollable if needed). Bottom = keyboard. After 90° rotation, logical
     * height is often ~320px; keep the password field directly under the title so it stays in the
     * upper area and is not squeezed against the keyboard (previous order put it last).
     */
    lv_obj_t *body = lv_obj_create(s_modal);
    lv_obj_set_size(body, hor, ver - kb_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_top(body, 4, 0);
    lv_obj_set_style_pad_bottom(body, 6, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    s_pwd_layout.body = body;
    s_pwd_layout.ver_full = ver;
    s_pwd_layout.kb_band = kb_h;

    lv_obj_t *panel = lv_obj_create(body);
    lv_obj_set_width(panel, LV_PCT(92));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_style_text_font(title, ui_font_get(), 0);
    lv_label_set_text_fmt(title, "Password for\n%s", ssid);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(100));
    /* Keep long SSIDs from consuming the whole band above the keyboard */
    lv_obj_set_style_max_height(title, ver / 3, 0);

    s_pwd_ta = lv_textarea_create(panel);
    lv_obj_set_width(s_pwd_ta, LV_PCT(100));
    lv_obj_set_height(s_pwd_ta, 48);
    lv_obj_set_style_text_font(s_pwd_ta, ui_font_get(), 0);
    lv_obj_set_style_pad_ver(s_pwd_ta, 8, 0);
    lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pwd_ta, 1, 0);
    lv_obj_set_style_border_color(s_pwd_ta, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_radius(s_pwd_ta, 4, 0);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_textarea_set_password_mode(s_pwd_ta, true);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Password (empty if open)");
    lv_obj_add_event_cb(s_pwd_ta, pwd_ta_clicked_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_ok = lv_btn_create(row);
    lv_obj_set_height(btn_ok, 44);
    lv_obj_set_flex_grow(btn_ok, 1);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_obj_set_style_text_font(lbl_ok, ui_font_get(), 0);
    lv_label_set_text(lbl_ok, "Connect");
    lv_obj_center(lbl_ok);
    lv_obj_add_event_cb(btn_ok, connect_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_cancel = lv_btn_create(row);
    lv_obj_set_height(btn_cancel, 44);
    lv_obj_set_flex_grow(btn_cancel, 1);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_obj_set_style_text_font(lbl_cancel, ui_font_get(), 0);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);
    lv_obj_add_event_cb(btn_cancel, cancel_btn_cb, LV_EVENT_CLICKED, nullptr);

    s_kb = lv_keyboard_create(s_modal);
    lv_obj_set_size(s_kb, hor, kb_h);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, ui_font_get(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_kb, ui_font_get(), LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, kb_ready_hide_cb, LV_EVENT_READY, nullptr);
    lv_keyboard_set_textarea(s_kb, s_pwd_ta);
    /* Ensure field sits in the scroll viewport (above keyboard band) */
    lv_obj_scroll_to_view(s_pwd_ta, LV_ANIM_OFF);
}

static void list_item_cb(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_results.size()) {
        return;
    }
    std::string pwd;
    if (wifi_store_find_password(s_results[idx].ssid.c_str(), &pwd)) {
        s_pending_ssid = s_results[idx].ssid;
        s_pending_password = pwd.c_str();
        s_pending_connect = true;
        set_status_text("Connecting...");
        return;
    }
    show_password_dialog(s_results[idx].ssid.c_str());
}

static void rebuild_list_cb(void *user_data) {
    LV_UNUSED(user_data);
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);

    if (s_results.empty()) {
        lv_obj_t *hint = lv_list_add_btn(s_list, LV_SYMBOL_WARNING, "No networks - tap Scan");
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    } else {
        for (size_t i = 0; i < s_results.size(); i++) {
            const ScanResult &r = s_results[i];
            /* Leading index helps when SimSun CJK subset omits rare SSID characters. */
            const std::string line = "#" + std::to_string(i) + " " + r.ssid + " (" + std::to_string(r.rssi) + " dBm)";
            lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_WIFI, line.c_str());
            lv_obj_set_style_text_font(btn, ui_font_get(), LV_PART_MAIN);
            for (uint32_t ch = 0; ch < lv_obj_get_child_cnt(btn); ch++) {
                lv_obj_set_style_text_font(lv_obj_get_child(btn, ch), ui_font_get(), 0);
            }
            lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        }
    }

    set_status_text("Tap a network, enter password, Connect");
    if (s_scan_btn) {
        lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
    }
}

static void scan_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_scan_busy || s_waiting_scan_complete) {
        return;
    }
    s_scan_requested = true;
    set_status_text("Scanning...");
    if (s_scan_btn) {
        lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    }
}

/** Collect APs after async scan completes (ssid is proper UTF-8 from driver). */
static std::vector<ScanResult> collect_and_sort(void) {
    std::vector<ScanResult> out;
    int n = 0;
    const wifi_ap_record_t *aps = wifi_net_scan_results(&n);
    for (int i = 0; i < n; i++) {
        ScanResult r;
        r.ssid = (const char *)aps[i].ssid;
        if (r.ssid.length() == 0) {
            continue;
        }
        r.rssi = aps[i].rssi;
        r.enc_type = (uint8_t)aps[i].authmode;
        (void)r.enc_type;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const ScanResult &a, const ScanResult &b) {
        return a.rssi > b.rssi;
    });
    /* Cap rows: huge lists + flex layout deep recursion can overflow the LVGL task stack. */
    static const size_t kMaxApRows = 32;
    if (out.size() > kMaxApRows) {
        out.resize(kMaxApRows);
    }
    return out;
}

/**
 * Publish scan results and schedule the list rebuild. Runs on the Arduino loop task, so it must
 * hold the LVGL lock: s_results is read by LVGL callbacks and lv_async_call touches LVGL timers.
 */
static void publish_results(std::vector<ScanResult> &&results) {
    bsp_display_lock(0);
    s_results = std::move(results);
    lv_async_call(rebuild_list_cb, nullptr);
    bsp_display_unlock();
}

/** Start non-blocking scan - avoids task WDT reset from multi-second blocking scan. */
static void start_async_scan(void) {
    s_scan_busy = true;
    s_waiting_scan_complete = false;

    if (!wifi_net_scan_start(true /*hidden*/, 120 /*ms/channel*/)) {
        s_scan_busy = false;
        publish_results({});
        return;
    }
    s_waiting_scan_complete = true;
}

/** Poll async scan; call from loop() every iteration. */
static void poll_async_scan(void) {
    if (!s_waiting_scan_complete) {
        return;
    }

    const int c = wifi_net_scan_poll();
    if (c == -1) {
        return;
    }

    s_waiting_scan_complete = false;
    s_scan_busy = false;

    if (c < 0) {
        publish_results({});
        return;
    }

    publish_results(collect_and_sort());
}

/** "Connected: <ssid>\nIP a.b.c.d" for the status label. */
static std::string connected_msg(void) {
    char ssid[33], ip[16];
    wifi_net_ssid(ssid, sizeof(ssid));
    wifi_net_ip(ip, sizeof(ip));
    return std::string("Connected: ") + ssid + "\nIP " + ip;
}

static void poll_connection(void) {
    bool connected = wifi_net_connected();

    if (s_conn == ConnState::Connecting) {
        if (wifi_store_autoconnect_active()) {
            wifi_store_autoconnect_poll();
            connected = wifi_net_connected();
            if (connected) {
                s_conn = ConnState::Connected;
                char ssid[33];
                wifi_net_ssid(ssid, sizeof(ssid));
                wifi_store_promote(ssid);
                std::string msg = connected_msg();
                bsp_display_lock(0);
                set_status_string(msg);
                if (s_status_lbl) {
                    lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
                }
                bsp_display_unlock();
                s_connect_start_ms = 0;
                clock_ui_on_wifi_connected();
                return;
            }
            if (!wifi_store_autoconnect_active()) {
                s_conn = ConnState::Failed;
                bsp_display_lock(0);
                set_status_text("Saved WiFi failed - tap Scan");
                if (s_status_lbl) {
                    lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
                }
                bsp_display_unlock();
                s_connect_start_ms = 0;
            }
            return;
        }

        if (connected) {
            s_conn = ConnState::Connected;
            if (s_connect_from_manual) {
                wifi_store_save(s_attempt_ssid.c_str(), s_attempt_password.c_str());
                s_connect_from_manual = false;
            }
            std::string msg = connected_msg();
            bsp_display_lock(0);
            set_status_string(msg);
            if (s_status_lbl) {
                lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
            }
            bsp_display_unlock();
            s_connect_start_ms = 0;
            clock_ui_on_wifi_connected();
            return;
        }
        if (now_ms() - s_connect_start_ms > 20000) {
            s_conn = ConnState::Failed;
            bsp_display_lock(0);
            set_status_text("Connection failed / timeout");
            if (s_status_lbl) {
                lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
            }
            bsp_display_unlock();
            s_connect_start_ms = 0;
        }
        return;
    }

    if (s_conn == ConnState::Connected && !connected) {
        s_conn = ConnState::Idle;
        clock_ui_on_wifi_lost();
        bsp_display_lock(0);
        set_status_text("WiFi lost - reconnecting...");
        if (s_status_lbl) {
            lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
        }
        bsp_display_unlock();
    } else if (s_conn != ConnState::Connected && connected) {
        /* Back by itself: wifi_net reconnects after a lost connection. */
        s_conn = ConnState::Connected;
        std::string msg = connected_msg();
        bsp_display_lock(0);
        set_status_string(msg);
        bsp_display_unlock();
        clock_ui_on_wifi_connected();
    }
}

void wifi_ui_init(void) {
    /* ESP-IDF Wi-Fi: STA mode, no power-save, credentials only on SD (see wifi_net.h). */
    wifi_net_init();

    /* Own screen (opened from the home screen); built now so status updates land even while hidden. */
    s_wifi_scr = lv_obj_create(nullptr);

    lv_obj_t *root = lv_obj_create(s_wifi_scr);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 8, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    (void)status_bar_create(root, "WiFi setup", true);

    s_status_lbl = lv_label_create(root);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_status_lbl, LV_PCT(100));
    lv_obj_set_style_text_font(s_status_lbl, ui_font_get(), 0);
    set_status_text("Tap Scan to find networks");

    s_list = lv_list_create(root);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_text_font(s_list, ui_font_get(), LV_PART_MAIN);

    s_scan_btn = lv_btn_create(root);
    lv_obj_set_width(s_scan_btn, LV_PCT(100));
    lv_obj_t *scan_lbl = lv_label_create(s_scan_btn);
    lv_obj_set_style_text_font(scan_lbl, ui_font_get(), 0);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " Scan networks");
    lv_obj_center(scan_lbl);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, nullptr);
}

void wifi_ui_open(void) {
    if (s_wifi_scr) {
        lv_scr_load(s_wifi_scr);
    }
}

void wifi_ui_loop(void) {
    wifi_net_poll(); /* Wi-Fi event log */

    if (!s_wifi_auto_started) {
        s_wifi_auto_started = true;
        wifi_store_begin();
        if (wifi_store_has_entries()) {
            wifi_store_autoconnect_start();
            s_conn = ConnState::Connecting;
            s_connect_start_ms = now_ms();
            s_connect_from_manual = false;
            bsp_display_lock(0);
            set_status_text("Connecting to saved WiFi...");
            bsp_display_unlock();
        }
    }

    if (s_pending_connect) {
        /* s_pending_* are written by LVGL event callbacks; copy them while holding the LVGL lock. */
        bsp_display_lock(0);
        s_pending_connect = false;
        s_attempt_ssid = s_pending_ssid;
        s_attempt_password = s_pending_password;
        s_pending_password = "";
        bsp_display_unlock();
        s_connect_from_manual = true;

        if (s_attempt_ssid.length() == 0) {
            s_conn = ConnState::Failed;
            s_connect_from_manual = false;
            bsp_display_lock(0);
            set_status_text("Invalid SSID");
            bsp_display_unlock();
            return;
        }

        /* Drops the current association (radio stays on), then connects. */
        (void)wifi_net_connect(s_attempt_ssid.c_str(), s_attempt_password.c_str(), 0, nullptr);
        s_conn = ConnState::Connecting;
        s_connect_start_ms = now_ms();
    }

    poll_async_scan();

    if (s_scan_requested && !s_scan_busy && !s_waiting_scan_complete) {
        s_scan_requested = false;
        start_async_scan();
    }
    poll_connection();
}
