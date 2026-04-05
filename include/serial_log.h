#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Call once at startup before other logs (115200, delay for USB). */
void serial_log_init(void);

/** Print to both ROM console (USB Serial/JTAG) and Arduino Serial; safe vsnprintf. */
void serial_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
