#include "time_sync.h"

#include <esp_sntp.h>
#include <stdlib.h>
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
     * ESP-IDF SNTP (what Arduino's configTzTime wrapped): poll mode, two servers. The esp_sntp_*
     * calls run on the lwIP thread, so no core lock is needed here. Server names are kept by
     * pointer: string literals only. TZ is already set (restored from SD by clock_ui); tzset()
     * just makes sure localtime() uses it.
     */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    tzset();
    const char *tz = getenv("TZ");
    serial_log("[time] NTP configured (TZ %s)", (tz && tz[0]) ? tz : "UTC");
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
