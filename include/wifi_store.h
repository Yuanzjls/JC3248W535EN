#pragma once

#include <string>

/**
 * Load known networks from SD (call after the SD card is mounted). Safe if SD missing or file absent.
 */
void wifi_store_begin(void);

/** True if any saved network exists (for auto-connect / known-network checks). */
bool wifi_store_has_entries(void);

/**
 * If password for this UTF-8 SSID was saved, copy to *out_pass and return true.
 * Open networks are stored with an empty password string.
 */
bool wifi_store_find_password(const char *ssid, std::string *out_pass);

/**
 * Save or update credentials and move this SSID to the front (tried first on next boot).
 */
void wifi_store_save(const char *ssid, const char *password);

/** Move an existing entry to the front after a successful connect (password unchanged). */
void wifi_store_promote(const char *ssid);

/**
 * Start trying saved networks in order (first = most recently saved). Non-blocking; call
 * wifi_store_autoconnect_poll() from loop().
 */
bool wifi_store_autoconnect_start(void);

/** Drive multi-step auto-connect; returns true while still attempting. */
bool wifi_store_autoconnect_poll(void);

/** True while auto-connect is cycling saved networks (use to skip the 20s manual timeout). */
bool wifi_store_autoconnect_active(void);
