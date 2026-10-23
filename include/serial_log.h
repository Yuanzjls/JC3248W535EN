#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Call once at startup before other logs (115200, delay for USB). */
void serial_log_init(void);

/** printf-style log line with a millisecond timestamp, to the USB Serial/JTAG port. */
void serial_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
