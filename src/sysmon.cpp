#include "sysmon.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "serial_log.h"

/*
 * CPU usage from FreeRTOS run-time stats (enabled in the Arduino-ESP32 build, counted in esp_timer
 * microseconds). Each second: busy % of a core = 100 - (its idle task's run time / wall time).
 * Per-task shares come from the same counters, matched by task handle between samples.
 */

static const int k_max_tasks = 40;
static const uint32_t k_sample_ms = 1000;
static const uint32_t k_log_ms = 60000;

struct PrevCounter {
    TaskHandle_t handle;
    uint32_t counter;
};

static SysStats s_stats;
static TaskStatus_t s_tasks[k_max_tasks];
static PrevCounter s_prev[k_max_tasks];
static int s_prev_n = 0;
static int64_t s_prev_us = 0;
static uint32_t s_last_log_ms = 0;

static uint32_t prev_counter(TaskHandle_t h, bool *found) {
    for (int i = 0; i < s_prev_n; i++) {
        if (s_prev[i].handle == h) {
            *found = true;
            return s_prev[i].counter;
        }
    }
    *found = false;
    return 0;
}

static void sample_memory(void) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_stats.heap_total = heap_caps_get_total_size(caps);
    s_stats.heap_free = heap_caps_get_free_size(caps);
    s_stats.heap_min = heap_caps_get_minimum_free_size(caps);
    s_stats.heap_largest = heap_caps_get_largest_free_block(caps);
    s_stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s_stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_stats.psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
}

static void sample_cpu(void) {
    const int64_t now_us = esp_timer_get_time();
    uint32_t total = 0;
    const int n = (int)uxTaskGetSystemState(s_tasks, k_max_tasks, &total);
    const int64_t wall_us = now_us - s_prev_us;
    const bool have_prev = s_prev_n > 0 && wall_us > 0;

    TaskHandle_t idle[2] = {xTaskGetIdleTaskHandleForCore(0), xTaskGetIdleTaskHandleForCore(1)};

    /* Top 3 non-idle tasks by share of one core */
    struct Top {
        const char *name;
        float pct;
    } top[3] = {{nullptr, 0}, {nullptr, 0}, {nullptr, 0}};

    for (int i = 0; i < n && have_prev; i++) {
        bool found = false;
        const uint32_t before = prev_counter(s_tasks[i].xHandle, &found);
        if (!found) {
            continue; /* task created since the last sample */
        }
        /* u32 microsecond counters wrap every ~71 min; unsigned subtraction handles one wrap. */
        const uint32_t delta = s_tasks[i].ulRunTimeCounter - before;
        const float pct = 100.0f * (float)delta / (float)wall_us;

        if (s_tasks[i].xHandle == idle[0] || s_tasks[i].xHandle == idle[1]) {
            const int core = s_tasks[i].xHandle == idle[0] ? 0 : 1;
            s_stats.cpu[core] = fmaxf(0.0f, fminf(100.0f, 100.0f - pct));
            continue;
        }
        for (int t = 0; t < 3; t++) {
            if (pct > top[t].pct) {
                for (int m = 2; m > t; m--) {
                    top[m] = top[m - 1];
                }
                top[t] = {s_tasks[i].pcTaskName, pct};
                break;
            }
        }
    }

    if (have_prev) {
        int len = 0;
        s_stats.top[0] = '\0';
        for (int t = 0; t < 3 && top[t].name; t++) {
            len += snprintf(s_stats.top + len, sizeof(s_stats.top) - len, "%s%s %.0f%%", t ? ", " : "", top[t].name,
                            top[t].pct);
            if (len >= (int)sizeof(s_stats.top)) {
                break;
            }
        }
        s_stats.valid = true;
    }

    s_prev_n = n;
    for (int i = 0; i < n; i++) {
        s_prev[i] = {s_tasks[i].xHandle, s_tasks[i].ulRunTimeCounter};
    }
    s_prev_us = now_us;
}

static void log_summary(void) {
    serial_log("[sys] CPU0 %.0f%%  CPU1 %.0f%%  |  RAM %lu/%lu KB used (min free %lu KB, block %lu KB)  |  "
               "PSRAM %lu/%lu KB used",
               s_stats.cpu[0], s_stats.cpu[1], (unsigned long)((s_stats.heap_total - s_stats.heap_free) / 1024),
               (unsigned long)(s_stats.heap_total / 1024), (unsigned long)(s_stats.heap_min / 1024),
               (unsigned long)(s_stats.heap_largest / 1024),
               (unsigned long)((s_stats.psram_total - s_stats.psram_free) / 1024),
               (unsigned long)(s_stats.psram_total / 1024));
    serial_log("[sys] top tasks: %s", s_stats.top);
}

static void sysmon_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    sample_cpu();
    sample_memory();
    const uint32_t now = millis();
    if (s_stats.valid && now - s_last_log_ms >= k_log_ms) {
        s_last_log_ms = now;
        log_summary();
    }
}

void sysmon_begin(void) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    sample_cpu(); /* baseline; first real numbers after one interval */
    sample_memory();
    s_last_log_ms = millis();
    (void)lv_timer_create(sysmon_timer_cb, k_sample_ms, nullptr);
}

const SysStats &sysmon_get(void) {
    return s_stats;
}
