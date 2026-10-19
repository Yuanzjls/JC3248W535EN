#pragma once

#include <esp_http_server.h>

/*
 * SD card file manager for the web page: list folders, download, upload (streamed to the card,
 * any size that fits), delete, create folders. No login (local network only).
 */

/** Add the /api/files, /api/file and /api/mkdir handlers to the running server. */
void web_files_register(httpd_handle_t server);
