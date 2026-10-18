
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "serial_log.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "ui_font.h"
#include "sd_card.h"
#include "clock_ui.h"
#include "wifi_store.h"
#include "wifi_ui.h"
#include "home_ui.h"
#include "weather.h"
#include "app_settings.h"
#include "airthings.h"
#include "sysmon.h"



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
static void ui_watchdog_task(void *arg);

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

/*
 * Redraw statistics (LVGL monitor callback, runs on the LVGL task). Every redraw is a full frame
 * on this panel, so this shows what an animated screen costs. Logged every 5 s, only when busy
 * (>= 5 redraws/s or a redraw over 100 ms), to keep idle logs quiet.
 */
static uint32_t s_rf_count, s_rf_total_ms, s_rf_max_ms, s_rf_window_start;

static void disp_monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
  LV_UNUSED(drv);
  LV_UNUSED(px);
  s_rf_count++;
  s_rf_total_ms += time_ms;
  if (time_ms > s_rf_max_ms) {
    s_rf_max_ms = time_ms;
  }
  const uint32_t now = millis();
  const uint32_t window = now - s_rf_window_start;
  if (window < 5000) {
    return;
  }
  if (s_rf_count >= 25 || s_rf_max_ms >= 100) {
    serial_log("[perf] %lu redraws in %lu ms (%.1f fps), avg %lu ms, max %lu ms", (unsigned long)s_rf_count,
               (unsigned long)window, s_rf_count * 1000.0f / window, (unsigned long)(s_rf_total_ms / s_rf_count),
               (unsigned long)s_rf_max_ms);
  }
  s_rf_count = s_rf_total_ms = s_rf_max_ms = 0;
  s_rf_window_start = now;
}

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

  BOOT_STEP("SD mount", (void)sd_card_mount());
  BOOT_STEP("wifi_store_begin (read SD)", wifi_store_begin());
  BOOT_STEP("clock_ui_init (read TZ)", clock_ui_init());
  BOOT_STEP("settings_load (read SD)", settings_load());

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
  /*
   * Pin LVGL to core 0. The panel needs full-frame refreshes, so an animated screen keeps the LVGL
   * task (priority 4) busy; unpinned, it also took core 1 and starved Arduino loop() (priority 1),
   * which showed up as "[loop] SLOW: ... blocked" and delayed WiFi handling. WiFi's own tasks on
   * core 0 have far higher priority, so they are unaffected.
   */
  cfg.lvgl_port_cfg.task_affinity = 0;

  BOOT_STEP("display start", bsp_display_start_with_config(&cfg));
  lv_disp_get_default()->driver->monitor_cb = disp_monitor_cb;
  BOOT_STEP("backlight on", bsp_display_brightness_set(settings_get().brightness));

  /* Lock the mutex due to the LVGL APIs are not thread-safe */
  BOOT_STEP("LVGL lock", bsp_display_lock(0));

  BOOT_STEP("font load from SD", (void)ui_font_load_from_sd());
  BOOT_STEP("wifi_ui_init", wifi_ui_init());
  BOOT_STEP("home_ui_init", home_ui_init()); /* default screen: app launcher */
  sysmon_begin(); /* CPU + memory sampling, [sys] log every 60 s */
  /* Small internal stack, core 1, above loop(): must keep running when the UI is stuck. */
  xTaskCreatePinnedToCore(ui_watchdog_task, "ui_wdog", 3072, nullptr, 3, nullptr, 1);

  bsp_display_unlock();

  /* Background weather fetch (waits for WiFi on its own). */
  weather_begin();
  airthings_begin(); /* BLE radon sensor, if one was chosen (/airthings.ini) */

  serial_log("[boot] setup done in %lu ms total (free heap %lu, free PSRAM %lu)", (unsigned long)(millis() - s_boot_t0),
             (unsigned long)ESP.getFreeHeap(), (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  lvgl_port_enable_stall_report(); /* boot held the display lock for seconds: not a stall */
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

/*
 * UI watchdog. The UI froze once on the Home screen: the LVGL task stopped while holding the
 * display lock (no more [sys] lines, weather blocked on the lock). Its own task - not loop(),
 * which also takes the lock (WiFi screen updates) and would freeze with it. Logs where the LVGL
 * task is stuck and who holds the lock after 5 s; restarts the board after 30 s so a wall display
 * never stays frozen (WiFi, settings and the sensor come back on their own).
 */
static void ui_watchdog_task(void *arg)
{
  LV_UNUSED(arg);
  static const uint32_t k_warn_ms = 5000;
  static const uint32_t k_restart_ms = 30000;
  uint32_t last_warn = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t stuck_ms = 0;
    int stage = 0;
    const char *holder = "";
    lvgl_port_debug(&stuck_ms, &stage, &holder);
    if (stuck_ms < k_warn_ms) {
      last_warn = 0;
      continue;
    }
    if (last_warn == 0 || stuck_ms - last_warn >= k_warn_ms) {
      last_warn = stuck_ms;
      serial_log("[ui] WATCHDOG: LVGL task stuck %lu s, %s (display lock held by: %s)",
                 (unsigned long)(stuck_ms / 1000), lvgl_port_stage_name(stage), holder);
    }
    if (stuck_ms >= k_restart_ms) {
      serial_log("[ui] WATCHDOG: UI frozen for %lu s - restarting", (unsigned long)(stuck_ms / 1000));
      delay(200); /* let the log line out */
      esp_restart();
    }
  }
}

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

  /*
   * The Arduino core calls loop() back-to-back with no pause, so without this it spins at 100 % of
   * core 1 doing nothing (core 1's idle task never runs). Everything polled here (WiFi status, scan
   * results, NTP) changes on a ~100 ms scale, so 5 ms of sleep costs no responsiveness.
   */
  delay(5);
}