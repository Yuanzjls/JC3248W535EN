#pragma once

#include <stdbool.h>

/** Call once when WiFi obtains an IP (STA connected). Safe to call multiple times (idempotent per connect). */
void time_sync_on_wifi_connected(void);

/** Call when WiFi disconnects so NTP can restart on next connect. */
void time_sync_on_wifi_disconnected(void);

/** Poll for NTP completion (non-blocking). Call from loop. */
void time_sync_poll(void);

/** True after UNIX time looks valid (NTP synced). */
bool time_sync_is_synced(void);
