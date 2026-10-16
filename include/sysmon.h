#pragma once

#include <stdint.h>

/** CPU and memory snapshot, refreshed once a second. */
struct SysStats {
    bool valid;
    float cpu[2]; /* % busy per core (100 - idle task share) */

    uint32_t heap_total; /* internal RAM, bytes */
    uint32_t heap_free;
    uint32_t heap_min;     /* lowest free since boot */
    uint32_t heap_largest; /* largest free block: small vs heap_free = fragmented */

    uint32_t psram_total;
    uint32_t psram_free;
    uint32_t psram_min;

    char top[96]; /* busiest tasks, e.g. "LVGL task 31%, loopTask 2%" */
};

/**
 * Start sampling (an LVGL timer, so call under bsp_display_lock after the display is up).
 * Uses FreeRTOS run-time stats; also logs a one-line summary to serial every 60 s.
 */
void sysmon_begin(void);

/** Latest snapshot (LVGL context). */
const SysStats &sysmon_get(void);
