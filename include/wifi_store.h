#pragma once

#include <Arduino.h>

/**
 * Load known networks from SD (call after SD.begin). Safe if SD missing or file absent.
 */
void wifi_store_begin(void);

/** True if any saved network exists (for auto-connect / known-network checks). */
bool wifi_store_has_entries(void);

/**
 * If password for this UTF-8 SSID was saved, copy to *out_pass and return true.
 * Open networks are stored with an empty password string.
 */
bool wifi_store_find_password(const String &ssid, String *out_pass);

/**
 * Save or update credentials and move this SSID to the front (tried first on next boot).
 */
void wifi_store_save(const String &ssid, const String &password);

/** Move an existing entry to the front after a successful connect (password unchanged). */
void wifi_store_promote(const String &ssid);

/**
 * Start trying saved networks in order (first = most recently saved). Non-blocking; call
 * wifi_store_autoconnect_poll() from loop().
 */
bool wifi_store_autoconnect_start(void);

/** Drive multi-step auto-connect; returns true while still attempting. */
bool wifi_store_autoconnect_poll(void);

/** True while auto-connect is cycling saved networks (use to skip the 20s manual timeout). */
bool wifi_store_autoconnect_active(void);
