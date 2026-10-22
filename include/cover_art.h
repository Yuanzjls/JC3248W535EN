#pragma once

#include <stdint.h>

/*
 * Album art for the Music app: decodes a picture embedded in an MP3 (JPEG or PNG bytes at
 * [offset, offset + len) of a card file) to RGB565 in a background task, scaled to fit a
 * size x size square. Safe to call from the LVGL task.
 */

/** Start decoding; replaces a request that hasn't started yet. `id` comes back with the result. */
void cover_art_request(const char *path, uint32_t offset, uint32_t len, int size, uint32_t id);

/**
 * Take a finished decode. Returns false if none is ready. On success *buf is a w x h RGB565 image
 * (LVGL byte order) in PSRAM that the caller frees with heap_caps_free, or nullptr if decoding failed.
 */
bool cover_art_take(uint32_t *id, uint16_t **buf, int *w, int *h);
