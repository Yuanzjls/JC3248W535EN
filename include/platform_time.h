#pragma once

/*
 * Time helpers on ESP-IDF, replacing Arduino's millis() / delay() (same behaviour: milliseconds
 * since boot from esp_timer, wrapping after ~49 days; sleep = FreeRTOS delay).
 */

#include <stdint.h>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/** Milliseconds since boot. Compare with subtraction (now_ms() - t0) so the wrap is harmless. */
static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** Block the calling task for ms milliseconds (lets other tasks run). */
static inline void sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
