
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "serial_log.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "ui_font.h"
#include "wifi_store.h"
#include "wifi_ui.h"



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


void setup()
{
  serial_log_init();
  serial_log("[app] boot: starting");

  (void)ui_font_mount_sd();
  wifi_store_begin();

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

  bsp_display_start_with_config(&cfg);
  bsp_display_backlight_on();

  /* Lock the mutex due to the LVGL APIs are not thread-safe */
  bsp_display_lock(0);

  (void)ui_font_load_from_sd();
  wifi_ui_init();

  bsp_display_unlock();

}

void loop()
{
  static bool s_once;
  if (!s_once && millis() > 3000U) {
    s_once = true;
    serial_log("[app] loop: alive (if you see this, Serial + ROM log work)");
  }
  wifi_ui_loop();
}