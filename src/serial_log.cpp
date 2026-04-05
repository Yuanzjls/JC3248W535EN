#include "serial_log.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" void serial_log_init(void) {
    Serial.begin(115200);
    /* Host often opens the COM port after the chip boots; wait so early lines are not lost. */
    delay(800);
}

extern "C" void serial_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    const size_t len = strlen(buf);
    const bool need_nl = (len == 0 || buf[len - 1] != '\n');

    /* Single sink: USB CDC Serial (ROM + Serial both showed duplicate lines on some S3 setups). */
    Serial.print(buf);
    if (need_nl) {
        Serial.print("\n");
    }
    Serial.flush();
}
