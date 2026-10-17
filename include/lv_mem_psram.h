#pragma once

/*
 * LVGL allocator that prefers PSRAM (lv_conf.h LV_MEM_CUSTOM_*).
 *
 * Plain malloc() puts every allocation under 4 KB (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) in
 * internal RAM, and nearly every LVGL object, style and label text is that small, so the screens
 * were using up the scarce internal RAM that WiFi, BLE and task stacks need. Prefer PSRAM; fall
 * back to internal RAM only if PSRAM is full. (LVGL never DMAs from these: the draw buffer and
 * the panel transfer buffers are allocated separately in lv_port.c.)
 */
#include <esp_heap_caps.h>
#include <stddef.h>

#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static inline void *lv_psram_malloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, LV_PSRAM_CAPS, LV_INTERNAL_CAPS);
}

static inline void *lv_psram_realloc(void *p, size_t size) {
    return heap_caps_realloc_prefer(p, size, 2, LV_PSRAM_CAPS, LV_INTERNAL_CAPS);
}

static inline void lv_psram_free(void *p) {
    heap_caps_free(p);
}
