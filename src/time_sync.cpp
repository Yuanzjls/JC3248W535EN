#include "time_sync.h"

#include <Arduino.h>
#include <time.h>

#include "serial_log.h"
#include "wifi_net.h"

static bool s_ntp_configured = false;
static bool s_sntp_started = false; /* once per boot: SNTP keeps running across reconnects */
static bool s_synced = false;

void time_sync_on_wifi_connected(void) {
    if (!wifi_net_connected()) {
        return;
    }
    if (s_ntp_configured) {
        return;
    }
    s_ntp_configured = true;
    s_synced = false;
    if (s_sntp_started) {
        return; /* still running from the first connect; it retries on its own */
    }
    s_sntp_started = true;
    /*
     * configTime(0, 0, ...) would overwrite TZ with "UTC0" and wipe the timezone restored from SD.
     * configTzTime re-applies the current TZ instead. Copy it first: setenv may free the old string.
     */
    const char *cur = getenv("TZ");
    const String tz = (cur && cur[0]) ? String(cur) : String("UTC0");
    configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
    serial_log("[time] NTP configured (TZ %s)", tz.c_str());
}

void time_sync_on_wifi_disconnected(void) {
    s_ntp_configured = false;
    s_synced = false;
}

void time_sync_poll(void) {
    if (!s_ntp_configured || s_synced) {
        return;
    }
    if (!wifi_net_connected()) {
        return;
    }
    time_t now = time(nullptr);
    if (now > 1700000000) {
        s_synced = true;
        struct tm ti;
        localtime_r(&now, &ti);
        serial_log("[time] NTP synced (local %04d-%02d-%02d %02d:%02d:%02d)", (int)(ti.tm_year + 1900),
                   (int)(ti.tm_mon + 1), (int)ti.tm_mday, (int)ti.tm_hour, (int)ti.tm_min, (int)ti.tm_sec);
    }
}

bool time_sync_is_synced(void) {
    return s_synced;
}
