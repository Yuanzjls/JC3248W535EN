#pragma once

#include <esp_http_server.h>
#include <string>

/*
 * Firmware update over Wi-Fi: the web page uploads firmware.bin, which is written to the other
 * app slot (app0 / app1) while it arrives, checked, and booted. A new firmware starts "on
 * probation" (bootloader rollback): it confirms itself after a minute of normal running, and a
 * crash or reset before that boots the previous firmware again.
 */

/** Call early at boot: logs the running firmware and starts the probation timer after an update. */
void ota_boot(void);

/** Version of the running firmware (from git describe at build time), e.g. "v1.0.0-3-g1a2b3c4". */
const char *ota_version(void);

/** "app0" / "app1": the slot the running firmware is in. */
const char *ota_slot(void);

/** Add GET /api/ota (version info) and PUT /api/ota (upload firmware.bin) to the web server. */
void ota_web_register(httpd_handle_t server);
