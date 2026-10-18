#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_wifi_types.h>

/*
 * Wi-Fi station on ESP-IDF (esp_wifi + esp_netif), replacing Arduino's WiFi class.
 *
 * Behaviour kept from the Arduino version: radio stays on in STA mode, no modem power-save,
 * credentials never written to internal flash (they live on SD, see wifi_store), WPA2 minimum
 * when a password is given, and automatic reconnect after a lost connection (not after our own
 * disconnect or a wrong password).
 */

/** Bring up netif + Wi-Fi driver and start the station (no connect yet). Call once at boot. */
void wifi_net_init(void);

/** Print queued Wi-Fi events (call from loop(): the event task's stack is too small to log). */
void wifi_net_poll(void);

/** Associated and has an IP address. */
bool wifi_net_connected(void);

/** SSID of the current AP into out (empty if not connected). */
void wifi_net_ssid(char *out, size_t n);

/** "a.b.c.d" (empty if no IP). */
void wifi_net_ip(char *out, size_t n);

/** RSSI of the current AP in dBm (0 if not connected). */
int wifi_net_rssi(void);

/** Start an async active scan (100-max_ms_per_chan ms per channel). False if it could not start. */
bool wifi_net_scan_start(bool show_hidden, uint32_t max_ms_per_chan);

/** Scan state: -1 still running, -2 failed, >= 0 finished with that many APs. */
int wifi_net_scan_poll(void);

/** Results of the last finished scan (valid until the next scan starts). */
const wifi_ap_record_t *wifi_net_scan_results(int *count);

/** Connect (drops any current association first). channel 0 / bssid NULL: let the driver find it. */
bool wifi_net_connect(const char *ssid, const char *password, int channel, const uint8_t *bssid);

/** Drop the current association; the radio stays on. No auto-reconnect after this. */
void wifi_net_disconnect(void);
