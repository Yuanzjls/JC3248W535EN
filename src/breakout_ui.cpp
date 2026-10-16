#include "breakout_ui.h"

#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <math.h>

#include "serial_log.h"
#include "status_bar.h"
#include "ui_theme.h"

/*
 * Breakout. Everything runs on the LVGL task (an lv_timer drives the physics), so no locking is
 * needed. Bricks, paddle and ball are plain lv_objs: moving one only redraws its old and new
 * area, which is much cheaper than repainting the whole field each frame over QSPI.
 */

static const int k_rows = 6;
static const int k_cols = 10;
static const float k_margin = 8;      /* field edge to bricks */
static const float k_brick_gap = 4;
static const float k_brick_h = 14;
static const float k_brick_top = 28;  /* free space above the bricks */
static const float k_paddle_w = 84;
static const float k_paddle_h = 10;
static const float k_paddle_bottom = 16; /* gap under the paddle */
static const float k_ball = 10;          /* diameter */
static const float k_start_speed = 240;  /* px/s */
static const float k_max_speed = 520;
static const float k_max_step = 3;       /* px per physics sub-step, so the ball can't tunnel */
static const float k_max_bounce = 60 * (float)M_PI / 180; /* paddle edge deflection */
static const int k_lives = 3;
static const char *k_best_path = "/breakout.ini";

enum class State { Ready, Playing, Paused, GameOver };

struct Brick {
    lv_obj_t *obj;
    float x, y, w, h;
    int points;
    bool alive;
};

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_field = nullptr;
static lv_obj_t *s_paddle = nullptr;
static lv_obj_t *s_ball = nullptr;
static lv_obj_t *s_msg = nullptr;
static Brick s_bricks[k_rows * k_cols];

static float s_fw = 0, s_fh = 0;   /* field size */
static float s_px = 0;             /* paddle left edge */
static float s_bx = 0, s_by = 0;   /* ball centre */
static float s_vx = 0, s_vy = 0;   /* ball velocity, px/s */
static float s_speed = k_start_speed;
static int s_score = 0, s_lives = k_lives, s_level = 1, s_best = 0, s_alive = 0, s_hits = 0;
static State s_state = State::Ready;
static uint32_t s_last_tick = 0;
static lv_timer_t *s_game_timer = nullptr;
static float s_bw = 0; /* brick width, for grid lookups */

/* Auto play */
static const float k_ai_paddle_speed = 1100; /* px/s: fast, but visibly moving rather than teleporting */
static const uint32_t k_ai_launch_delay_ms = 700;
static const uint32_t k_ai_restart_delay_ms = 2500;
static bool s_auto = false;
static bool s_auto_used = false; /* auto played any part of this game: score doesn't count as best */
static uint32_t s_auto_wait_until = 0;
static lv_obj_t *s_auto_btn = nullptr;
static uint32_t s_last_hit_ms = 0;  /* last brick hit: detects endless no-progress loops */
static bool s_ai_planned = false;   /* aim chosen for the current descent */
static float s_ai_rel = 0;          /* chosen contact point on the paddle, -1..1 */
static const uint32_t k_stall_ms = 8000;
static const float k_min_bounce = 4 * (float)M_PI / 180;

/**
 * Paddle bounce angle for a contact point rel (-1..1). Never closer than 4 deg to vertical: a
 * dead-centre hit up an empty column would otherwise bounce straight up and down forever.
 * rel == 0 has no side, so the caller supplies one.
 */
static float bounce_angle(float rel, bool right_if_centre) {
    const float a = rel * k_max_bounce;
    if (fabsf(a) >= k_min_bounce) {
        return a;
    }
    if (a > 0 || (a == 0 && right_if_centre)) {
        return k_min_bounce;
    }
    return -k_min_bounce;
}

static void auto_wait(uint32_t ms) {
    s_auto_wait_until = lv_tick_get() + ms;
}

/** Message for a "ball on paddle" moment; the tap hint is only shown when a human is playing. */
static void show_msg(const char *text);
static void show_ready_msg(const char *headline) {
    char buf[96];
    snprintf(buf, sizeof(buf), s_auto ? "%s" : "%s\nTap to launch", headline);
    show_msg(buf);
    auto_wait(k_ai_launch_delay_ms);
}

/* ---------- persistence ---------- */

static void load_best(void) {
    File f = SD.open(k_best_path, FILE_READ);
    if (!f) {
        return;
    }
    String line = f.readStringUntil('\n');
    f.close();
    if (line.startsWith("best=")) {
        s_best = line.substring(5).toInt();
    }
}

static void save_best(void) {
    File f = SD.open(k_best_path, FILE_WRITE);
    if (!f) {
        serial_log("[breakout] cannot write %s", k_best_path);
        return;
    }
    f.printf("best=%d\n", s_best);
    f.close();
}

/* ---------- display helpers ---------- */

static float paddle_y(void) {
    return s_fh - k_paddle_bottom - k_paddle_h;
}

static void update_hud(void) {
    /* Title space is ~180 px between the Home button and the clock: keep it short.
     * Level and best score are shown in the field messages instead. */
    char buf[40];
    snprintf(buf, sizeof(buf), "%d pts   %d lives", s_score, s_lives);
    status_bar_set_title(s_bar, buf);
}

static void show_msg(const char *text) {
    lv_label_set_text(s_msg, text);
    lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_msg);
}

static void hide_msg(void) {
    lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
}

static void place_ball(void) {
    lv_obj_set_pos(s_ball, (lv_coord_t)(s_bx - k_ball / 2), (lv_coord_t)(s_by - k_ball / 2));
}

static void place_paddle(void) {
    lv_obj_set_pos(s_paddle, (lv_coord_t)s_px, (lv_coord_t)paddle_y());
}

/** Ball resting on the paddle, waiting for a tap. */
static void attach_ball(void) {
    s_bx = s_px + k_paddle_w / 2;
    s_by = paddle_y() - k_ball / 2 - 1;
    s_vx = s_vy = 0;
    place_ball();
}

static void set_velocity_angle(float angle) {
    s_vx = s_speed * sinf(angle);
    s_vy = -s_speed * cosf(angle);
}

/* ---------- game flow ---------- */

static void build_level(void) {
    for (auto &b : s_bricks) {
        b.alive = true;
        lv_obj_clear_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
    }
    s_alive = k_rows * k_cols;
    s_hits = 0;
    s_speed = fminf(k_start_speed * (1.0f + 0.12f * (s_level - 1)), k_max_speed);
}

static void new_game(void) {
    s_score = 0;
    s_lives = k_lives;
    s_level = 1;
    build_level();
    s_px = (s_fw - k_paddle_w) / 2;
    place_paddle();
    attach_ball();
    s_state = State::Ready;
    s_auto_used = s_auto;
    update_hud();
    char buf[64];
    snprintf(buf, sizeof(buf), s_auto ? "Auto play\nBest: %d" : "Drag to move\nBest: %d", s_best);
    show_ready_msg(buf);
}

static void launch(void) {
    /* Random-ish start angle so every serve isn't identical. */
    const float a = ((float)(esp_random() % 61) - 30.0f) * (float)M_PI / 180;
    set_velocity_angle(a);
    s_state = State::Playing;
    s_last_hit_ms = lv_tick_get();
    s_ai_planned = false;
    hide_msg();
}

static void lose_life(void) {
    s_lives--;
    update_hud();
    if (s_lives > 0) {
        s_state = State::Ready;
        attach_ball();
        show_ready_msg("Missed!");
        return;
    }
    s_state = State::GameOver;
    auto_wait(k_ai_restart_delay_ms);
    char buf[80];
    if (s_auto_used) {
        snprintf(buf, sizeof(buf), "Game over - score %d\n(auto play, not counted)", s_score);
    } else if (s_score > s_best) {
        s_best = s_score;
        save_best();
        update_hud();
        snprintf(buf, sizeof(buf), "New best: %d!\nTap to play again", s_score);
    } else {
        snprintf(buf, sizeof(buf), "Game over - score %d\nTap to play again", s_score);
    }
    show_msg(buf);
    serial_log("[breakout] game over, score %d (best %d)", s_score, s_best);
}

static void level_clear(void) {
    s_level++;
    build_level();
    attach_ball();
    s_state = State::Ready;
    update_hud();
    char buf[48];
    snprintf(buf, sizeof(buf), "Level %d", s_level);
    show_ready_msg(buf);
}

/** Gradual speed-up while a level is played: +4% every 8 bricks. */
static void bump_speed(void) {
    if (++s_hits % 8 != 0 || s_speed >= k_max_speed) {
        return;
    }
    const float ns = fminf(s_speed * 1.04f, k_max_speed);
    s_vx *= ns / s_speed;
    s_vy *= ns / s_speed;
    s_speed = ns;
}

/* ---------- physics ---------- */

/** Returns false if the step ended the rally (life lost or level cleared). */
static bool physics_substep(float dt) {
    const float r = k_ball / 2;
    s_bx += s_vx * dt;
    s_by += s_vy * dt;

    /* Walls */
    if (s_bx - r < 0) {
        s_bx = r;
        s_vx = fabsf(s_vx);
    } else if (s_bx + r > s_fw) {
        s_bx = s_fw - r;
        s_vx = -fabsf(s_vx);
    }
    if (s_by - r < 0) {
        s_by = r;
        s_vy = fabsf(s_vy);
    }
    if (s_by - r > s_fh) {
        lose_life();
        return false;
    }

    /* Paddle: bounce angle depends on where it hits (centre = near-vertical, edges = 60 deg). */
    const float py = paddle_y();
    if (s_vy > 0 && s_by + r >= py && s_by - r <= py + k_paddle_h && s_bx + r >= s_px &&
        s_bx - r <= s_px + k_paddle_w) {
        float rel = (s_bx - (s_px + k_paddle_w / 2)) / (k_paddle_w / 2);
        rel = fmaxf(-1.0f, fminf(1.0f, rel));
        set_velocity_angle(bounce_angle(rel, (esp_random() & 1) != 0));
        s_by = py - r;
    }

    /* Bricks: at most one per sub-step; reflect on the axis with the smaller overlap. */
    for (auto &b : s_bricks) {
        if (!b.alive || s_bx + r <= b.x || s_bx - r >= b.x + b.w || s_by + r <= b.y || s_by - r >= b.y + b.h) {
            continue;
        }
        const float ox = fminf(s_bx + r - b.x, b.x + b.w - (s_bx - r));
        const float oy = fminf(s_by + r - b.y, b.y + b.h - (s_by - r));
        if (ox < oy) {
            s_vx = -s_vx;
            s_bx = s_bx < b.x + b.w / 2 ? b.x - r : b.x + b.w + r;
        } else {
            s_vy = -s_vy;
            s_by = s_by < b.y + b.h / 2 ? b.y - r : b.y + b.h + r;
        }
        b.alive = false;
        s_last_hit_ms = lv_tick_get();
        lv_obj_add_flag(b.obj, LV_OBJ_FLAG_HIDDEN);
        s_score += b.points;
        update_hud();
        bump_speed();
        if (--s_alive == 0) {
            level_clear();
            return false;
        }
        break;
    }
    return true;
}

/* ---------- auto play ---------- */

/** Fold an unbounded x back into the field, mirroring off the side walls like the ball does. */
static float fold_x(float x) {
    const float r = k_ball / 2;
    const float span = s_fw - 2 * r;
    float u = fmodf(x - r, 2 * span);
    if (u < 0) {
        u += 2 * span;
    }
    if (u > span) {
        u = 2 * span - u;
    }
    return r + u;
}

/**
 * Where the ball centre will be when it reaches paddle height. Exact on the way down; on the way
 * up it assumes a ceiling bounce (bricks may change that, but it's recomputed every frame).
 */
static float predict_landing_x(void) {
    const float r = k_ball / 2;
    const float land_y = paddle_y() - r;
    const float vy = fabsf(s_vy);
    if (vy < 1) {
        return s_bx;
    }
    const float dist = s_vy > 0 ? land_y - s_by : (s_by - r) + (land_y - r);
    return fold_x(s_bx + s_vx * (dist / vy));
}

/** Index of the live brick containing (x, y), or -1. O(1) grid lookup. */
static int brick_at(float x, float y) {
    const float gx = x - k_margin;
    const float gy = y - k_brick_top;
    if (gx < 0 || gy < 0) {
        return -1;
    }
    const int c = (int)(gx / (s_bw + k_brick_gap));
    const int r = (int)(gy / (k_brick_h + k_brick_gap));
    if (c >= k_cols || r >= k_rows) {
        return -1;
    }
    if (gx - c * (s_bw + k_brick_gap) > s_bw || gy - r * (k_brick_h + k_brick_gap) > k_brick_h) {
        return -1; /* in the gap between bricks */
    }
    const int i = r * k_cols + c;
    return s_bricks[i].alive ? i : -1;
}

/**
 * Value of sending the ball up from (x, y) at angle a: the points of the first brick it would hit.
 * The trace follows the bounce off the top wall, so slipping through a gap and hitting bricks from
 * behind (the classic best move) earns a bonus. A shot that comes back down without touching
 * anything is worth 0: that's the "straight up an empty column" loop.
 */
static float trace_value(float x, float y, float a) {
    const float r = k_ball / 2;
    const float step = 4;
    const float wall_bottom = k_brick_top + k_rows * (k_brick_h + k_brick_gap);
    float dx = sinf(a), dy = -cosf(a);
    bool behind = false;
    for (int i = 0; i < 300; i++) {
        x += dx * step;
        y += dy * step;
        if (x < r) {
            x = r;
            dx = fabsf(dx);
        } else if (x > s_fw - r) {
            x = s_fw - r;
            dx = -fabsf(dx);
        }
        if (y < r) {
            y = r;
            dy = fabsf(dy);
            behind = true;
        }
        if (dy > 0 && y - r > wall_bottom) {
            return 0; /* fell back below the wall without hitting anything */
        }
        const int b = brick_at(x, dy < 0 ? y - r : y + r); /* leading edge */
        if (b >= 0) {
            return (float)s_bricks[b].points + (behind ? 30.0f : 0.0f);
        }
    }
    return 0;
}

/** Paddle left edge that makes the ball land at contact point rel (-1..1). */
static float paddle_left_for(float land, float rel) {
    /* rel = (ball_x - paddle_centre) / half_width  =>  left = ball_x - half_width * (1 + rel) */
    return land - (k_paddle_w / 2) * (1 + rel);
}

/**
 * Pick the contact point for this descent: the most valuable reachable brick, ties going to
 * straighter (safer) angles. If no shot reaches a brick, or nothing has been hit for a while
 * (some loop the trace didn't foresee), take a random sideways shot to break the pattern.
 */
static float choose_rel(float land) {
    const float launch_y = paddle_y() - k_ball / 2;
    const bool stalled = lv_tick_get() - s_last_hit_ms > k_stall_ms;
    float best_rel = 0.3f, best_score = 0;
    bool any_valid = false;
    for (int k = -8; k <= 8; k++) { /* skip the extreme edges (a late paddle could miss) and centre */
        if (k == 0) {
            continue;
        }
        const float rel = k / 10.0f;
        const float left = paddle_left_for(land, rel);
        if (left < 0 || left > s_fw - k_paddle_w) {
            continue;
        }
        any_valid = true;
        const float score = trace_value(land, launch_y, bounce_angle(rel, true)) - 6 * fabsf(rel);
        if (score > best_score) {
            best_score = score;
            best_rel = rel;
        }
    }
    if (stalled || best_score <= 0 || !any_valid) {
        static const float k_escape[] = {-0.6f, -0.4f, 0.4f, 0.6f};
        best_rel = k_escape[esp_random() % 4];
        if (stalled) {
            serial_log("[breakout] auto: no brick hit for %lu s, trying a new angle",
                       (unsigned long)(k_stall_ms / 1000));
            s_last_hit_ms = lv_tick_get(); /* give the new angle a fresh window */
        }
    }
    return best_rel;
}

/**
 * Paddle left edge to aim for. While the ball rises, wait under its predicted landing spot; once
 * it starts falling, choose the shot once (so the plan doesn't flicker) and line up for it.
 */
static float ai_target_paddle_left(void) {
    const float land = predict_landing_x();
    float rel = 0;
    if (s_vy <= 0) {
        s_ai_planned = false;
    } else {
        if (!s_ai_planned) {
            s_ai_rel = choose_rel(land);
            s_ai_planned = true;
        }
        rel = s_ai_rel;
    }
    return fmaxf(0, fminf(s_fw - k_paddle_w, paddle_left_for(land, rel)));
}

static void auto_tick(float dt) {
    const bool waited = (int32_t)(lv_tick_get() - s_auto_wait_until) >= 0;
    switch (s_state) {
    case State::Ready:
        if (waited) {
            launch();
        }
        return;
    case State::GameOver:
        if (waited) {
            new_game();
        }
        return;
    case State::Paused: /* back on screen: just carry on */
        s_state = State::Playing;
        hide_msg();
        break;
    case State::Playing:
        break;
    }

    const float target = ai_target_paddle_left();
    const float max_move = k_ai_paddle_speed * dt;
    s_px += fmaxf(-max_move, fminf(max_move, target - s_px));
    place_paddle();
}

static void auto_cb(lv_event_t *e) {
    LV_UNUSED(e);
    s_auto = lv_obj_has_state(s_auto_btn, LV_STATE_CHECKED);
    if (s_auto) {
        s_auto_used = true;
        auto_wait(k_ai_launch_delay_ms);
    }
    serial_log("[breakout] auto play %s", s_auto ? "on" : "off");
}

static void game_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    const uint32_t now = lv_tick_get();
    float dt = (now - s_last_tick) / 1000.0f;
    s_last_tick = now;
    if (dt > 0.05f) {
        dt = 0.05f; /* after a stall, don't teleport the ball */
    }

    if (lv_scr_act() != s_scr) {
        if (s_state == State::Playing) {
            s_state = State::Paused;
            show_msg("Paused - tap to resume");
        }
        lv_timer_pause(s_game_timer); /* no 16 ms wakeups while another app is open */
        return;
    }
    if (s_auto) {
        auto_tick(dt);
    }
    if (s_state != State::Playing) {
        return;
    }

    const int steps = (int)ceilf(s_speed * dt / k_max_step);
    const float sub = steps > 0 ? dt / steps : dt;
    for (int i = 0; i < steps; i++) {
        if (!physics_substep(sub)) {
            break;
        }
    }
    if (s_state == State::Playing) {
        place_ball();
    }
}

/* ---------- input ---------- */

static void move_paddle_to(lv_coord_t screen_x) {
    lv_area_t a;
    lv_obj_get_coords(s_field, &a);
    float x = (float)(screen_x - a.x1) - k_paddle_w / 2;
    s_px = fmaxf(0, fminf(s_fw - k_paddle_w, x));
    place_paddle();
    if (s_state == State::Ready) {
        attach_ball();
    }
}

static void field_event_cb(lv_event_t *e) {
    if (s_auto) {
        return; /* hands off while auto play is on; toggle it with the Auto button */
    }
    const lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);

    if (code == LV_EVENT_PRESSED) {
        switch (s_state) {
        case State::Ready:
            launch();
            break;
        case State::Paused:
            s_state = State::Playing;
            hide_msg();
            break;
        case State::GameOver:
            new_game();
            return; /* this tap only restarts; the next one launches */
        case State::Playing:
            break;
        }
    }
    move_paddle_to(p.x);
}

static void restart_cb(lv_event_t *e) {
    LV_UNUSED(e);
    new_game();
}

/* ---------- screen ---------- */

static lv_obj_t *make_block(lv_coord_t w, lv_coord_t h, lv_color_t color, lv_coord_t radius) {
    lv_obj_t *o = lv_obj_create(s_field);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); /* touches go to the field */
    return o;
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

    s_bar = status_bar_create(s_scr, "Breakout", true);
    /* "Auto" toggle: checkable, turns green while auto play is on. */
    s_auto_btn = status_bar_add_action(s_bar, "Auto", auto_cb, nullptr);
    lv_obj_set_width(s_auto_btn, 60);
    lv_obj_set_style_text_font(lv_obj_get_child(s_auto_btn, 0), &lv_font_montserrat_16, 0);
    lv_obj_add_flag(s_auto_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_auto_btn, lv_palette_main(LV_PALETTE_GREEN), LV_STATE_CHECKED);
    (void)status_bar_add_action(s_bar, LV_SYMBOL_REFRESH, restart_cb, nullptr);

    s_field = lv_obj_create(s_scr);
    ui_style_card(s_field);
    lv_obj_set_style_pad_all(s_field, 0, 0);
    lv_obj_set_width(s_field, LV_PCT(100));
    lv_obj_set_flex_grow(s_field, 1);
    lv_obj_clear_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_field, field_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_field, field_event_cb, LV_EVENT_PRESSING, nullptr);

    /* Field size is only known after flex layout. */
    lv_obj_update_layout(s_scr);
    s_fw = lv_obj_get_content_width(s_field);
    s_fh = lv_obj_get_content_height(s_field);

    static const lv_palette_t k_row_colors[k_rows] = {LV_PALETTE_RED,   LV_PALETTE_DEEP_ORANGE, LV_PALETTE_AMBER,
                                                      LV_PALETTE_GREEN, LV_PALETTE_BLUE,        LV_PALETTE_PURPLE};
    const float bw = (s_fw - 2 * k_margin - (k_cols - 1) * k_brick_gap) / k_cols;
    s_bw = bw;
    for (int r = 0; r < k_rows; r++) {
        for (int c = 0; c < k_cols; c++) {
            Brick &b = s_bricks[r * k_cols + c];
            b.x = k_margin + c * (bw + k_brick_gap);
            b.y = k_brick_top + r * (k_brick_h + k_brick_gap);
            b.w = bw;
            b.h = k_brick_h;
            b.points = (k_rows - r) * 10; /* top rows are worth more */
            b.obj = make_block((lv_coord_t)bw, (lv_coord_t)k_brick_h, lv_palette_main(k_row_colors[r]), 3);
            lv_obj_set_pos(b.obj, (lv_coord_t)b.x, (lv_coord_t)b.y);
        }
    }

    s_paddle = make_block((lv_coord_t)k_paddle_w, (lv_coord_t)k_paddle_h, ui_color_text(), LV_RADIUS_CIRCLE);
    s_ball = make_block((lv_coord_t)k_ball, (lv_coord_t)k_ball, lv_palette_main(LV_PALETTE_DEEP_ORANGE),
                        LV_RADIUS_CIRCLE);

    s_msg = lv_label_create(s_field);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_msg, ui_color_text(), 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_msg, ui_color_surface(), 0);
    lv_obj_set_style_bg_opa(s_msg, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_msg, 12, 0);
    lv_obj_set_style_pad_all(s_msg, 12, 0);
    lv_obj_set_style_shadow_width(s_msg, 12, 0);
    lv_obj_set_style_shadow_opa(s_msg, LV_OPA_20, 0);
    lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_CLICKABLE);

    load_best();
    new_game();

    s_last_tick = lv_tick_get();
    s_game_timer = lv_timer_create(game_timer_cb, 16, nullptr);
    serial_log("[breakout] field %dx%d, brick %.1fx%.0f", (int)s_fw, (int)s_fh, bw, k_brick_h);
}

void breakout_ui_open(void) {
    build_screen();
    s_last_tick = lv_tick_get();
    lv_timer_resume(s_game_timer);
    lv_scr_load(s_scr);
}
