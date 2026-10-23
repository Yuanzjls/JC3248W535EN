#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

/*
 * ArduinoJson allocator that puts documents in PSRAM (falling back to internal RAM). With the
 * default allocator, every node and string under 4 KB lands in scarce internal RAM
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). Use as: JsonDocument doc(&g_psram_json);
 */
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void deallocate(void *p) override {
        heap_caps_free(p);
    }
    void *reallocate(void *p, size_t n) override {
        return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
};

inline PsramJsonAllocator g_psram_json;
