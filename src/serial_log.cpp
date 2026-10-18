#include "serial_log.h"
#include "platform_time.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>

/*
 * Log output on the board's USB port (the S3's built-in USB Serial/JTAG), through ESP-IDF's
 * interrupt-driven driver with a 4 KB transmit buffer: serial_log() only copies the line into the
 * buffer and returns. ESP-IDF's own console output (ESP_LOGx, panic messages) is switched to the
 * same driver, so the two never write the USB FIFO at the same time.
 */
static bool s_driver_ok = false;

extern "C" void serial_log_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096; /* room for bursts of log lines */
    s_driver_ok = usb_serial_jtag_driver_install(&cfg) == ESP_OK;
    if (s_driver_ok) {
        usb_serial_jtag_vfs_use_driver();
    }
    /* Host often opens the COM port after the chip boots; wait so early lines are not lost. */
    sleep_ms(800);
}

extern "C" void serial_log(const char *fmt, ...) {
    char buf[256];
    /* Millisecond timestamp prefix: gaps between lines show where boot / loop time goes. */
    const int pre = snprintf(buf, sizeof(buf), "[%7lu] ", (unsigned long)now_ms());
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf + pre, sizeof(buf) - pre, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = '\0'; /* keep room for the newline */

    size_t len = strlen(buf);
    if (len == 0 || buf[len - 1] != '\n') {
        buf[len++] = '\n';
    }
    if (!s_driver_ok || !usb_serial_jtag_is_connected()) {
        return; /* no host: drop instead of filling the buffer and waiting on it */
    }
    /* Short wait only if the buffer is full (a burst); never block the caller for long. */
    (void)usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(20));
}
