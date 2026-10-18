#pragma once

/*
 * Small web page on the board (http://<board-IP>/): status (Wi-Fi, uptime, CPU/memory, weather,
 * radon) and settings (brightness, units, clock, timezone, weather location).
 *
 * Open on the local network, no login. Changes are only accepted as JSON requests (a browser
 * won't send those cross-site without the board's consent), and the page never shows Wi-Fi
 * passwords.
 */

/** Start the HTTP server (call once after wifi_ui_init(); it serves as soon as there is an IP). */
void web_server_start(void);
