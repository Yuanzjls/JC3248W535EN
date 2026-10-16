#pragma once

#include <Arduino.h>
#include <vector>

/** Image types the viewer can decode (by extension): JPEG, PNG, BMP. */
bool media_is_image(const String &name);

/** Video types the player can decode (by extension): raw MJPEG (.mjpeg / .mjpg). */
bool media_is_video(const String &name);

/**
 * Full-screen image viewer. `paths` are the folder's images (for prev/next and swipe), `index` the
 * one to show first. Returns to the current screen on Back. LVGL context / under bsp_display_lock.
 */
void media_ui_open_images(const std::vector<String> &paths, size_t index);

/** Full-screen MJPEG player (loops). Returns to the current screen on Back. */
void media_ui_open_video(const String &path);
