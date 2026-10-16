
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "serial_log.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "ui_font.h"
#include "clock_ui.h"
#include "wifi_store.h"
#include "wifi_ui.h"
#include "home_ui.h"
#include "weather.h"



/**
 * @brief LVGL porting example
 * Set the rotation degree:
 *      - 0: 0 degree
 *      - 90: 90 degree
 *      - 180: 180 degree
 *      - 270: 270 degree
 *
 */
#define LVGL_PORT_ROTATION_DEGREE (90)

void setup();

/** Why the chip last restarted - tells a crash (panic/WDT/brownout) apart from a power cycle. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
  switch (r) {
  case ESP_RST_POWERON: return "power-on";
  case ESP_RST_SW: return "software restart";
  case ESP_RST_PANIC: return "PANIC (crash)";
  case ESP_RST_INT_WDT: return "interrupt watchdog";
  case ESP_RST_TASK_WDT: return "task watchdog";
  case ESP_RST_WDT: return "other watchdog";
  case ESP_RST_BROWNOUT: return "BROWNOUT (power dip)";
  case ESP_RST_DEEPSLEEP: return "deep sleep wake";
  case ESP_RST_USB: return "USB reset";
  case ESP_RST_JTAG: return "JTAG reset";
  case ESP_RST_EXT: return "external pin";
  default: return "unknown";
  }
}

static uint32_t s_boot_t0;

/** Run one boot step and log how long it took, so a slow start can be pinned to a step. */
#define BOOT_STEP(name, expr)                                                                    \
  do {                                                                                           \
    serial_log("[boot] %s ...", name);                                                           \
    const uint32_t _t = millis();                                                                \
    expr;                                                                                        \
    serial_log("[boot] %s done in %lu ms", name, (unsigned long)(millis() - _t));               \
  } while (0)

void setup()
{
  s_boot_t0 = millis();
  serial_log_init();
  serial_log("[app] boot: starting (reset reason %d: %s)", (int)esp_reset_reason(), reset_reason_str(esp_reset_reason()));

  BOOT_STEP("SD mount", (void)ui_font_mount_sd());
  BOOT_STEP("wifi_store_begin (read SD)", wifi_store_begin());
  BOOT_STEP("clock_ui_init (read TZ)", clock_ui_init());

  size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
      .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 270
      .rotate = LV_DISP_ROT_270,
#elif LVGL_PORT_ROTATION_DEGREE == 180
      .rotate = LV_DISP_ROT_180,
#elif LVGL_PORT_ROTATION_DEGREE == 0
      .rotate = LV_DISP_ROT_NONE,
#endif
  };
  /* Default 4096 is too small for WiFi list + flex layout + CJK fonts (stack canary crash). */
  cfg.lvgl_port_cfg.task_stack = 16384;

  BOOT_STEP("display start", bsp_display_start_with_config(&cfg));
  BOOT_STEP("backlight on", bsp_display_backlight_on());

  /* Lock the mutex due to the LVGL APIs are not thread-safe */
  BOOT_STEP("LVGL lock", bsp_display_lock(0));

  BOOT_STEP("font load from SD", (void)ui_font_load_from_sd());
  BOOT_STEP("wifi_ui_init", wifi_ui_init());
  BOOT_STEP("home_ui_init", home_ui_init()); /* default screen: app launcher */

  bsp_display_unlock();

  /* Background weather fetch (waits for WiFi on its own). */
  weather_begin();

  serial_log("[boot] setup done in %lu ms total (free heap %lu, free PSRAM %lu)", (unsigned long)(millis() - s_boot_t0),
             (unsigned long)ESP.getFreeHeap(), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/** Time one loop step; log it if it blocks the loop for longer than k_slow_step_ms. */
static const uint32_t k_slow_step_ms = 200;
#define LOOP_STEP(name, expr)                                                                    \
  do {                                                                                           \
    const uint32_t _t = millis();                                                                \
    expr;                                                                                        \
    const uint32_t _d = millis() - _t;                                                           \
    if (_d > k_slow_step_ms) {                                                                   \
      serial_log("[loop] SLOW: %s blocked %lu ms", name, (unsigned long)_d);                    \
    }                                                                                            \
  } while (0)

void loop()
{
  static bool s_once;
  static uint32_t s_last_loop_ms;
  const uint32_t now = millis();
  if (!s_once) {
    s_once = true;
    serial_log("[loop] first loop() at %lu ms after power-on", (unsigned long)now);
  } else if (now - s_last_loop_ms > k_slow_step_ms) {
    /* Gap between loop() calls (time spent outside our steps, e.g. other tasks starving loop) */
    serial_log("[loop] SLOW: %lu ms between loop() calls", (unsigned long)(now - s_last_loop_ms));
  }

  LOOP_STEP("wifi_ui_loop", wifi_ui_loop());
  LOOP_STEP("clock_ui_loop", clock_ui_loop());
  s_last_loop_ms = millis();
}