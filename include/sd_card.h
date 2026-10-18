#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SD card through ESP-IDF's own driver (sdspi on SPI3 + FATFS at "/sd"), instead of the Arduino SD
 * library. Data blocks move by DMA while the calling task sleeps; the Arduino library busy-waited
 * on every byte (core 0 at ~90 % during video, a watchdog reset, a starved UI).
 *
 * File access keeps the Arduino API: sd_fs().open(), File.read(), openNextFile()... (an fs::FS
 * over the ESP-IDF mount), so callers only swap SD for sd_fs().
 */

/** Mount at "/sd" (call once at boot). Recovers a card left mid-transfer by a reset. */
bool sd_card_mount(void);

bool sd_card_mounted(void);

/** Arduino-style file system on the card (paths relative to the card root, e.g. "/settings.ini"). */
fs::FS &sd_fs(void);

/** "SDHC", "SDSC", "MMC", or "none". */
const char *sd_card_type(void);

/** Card capacity in bytes (0 if not mounted). */
uint64_t sd_card_size_bytes(void);

/** File system size and used space in bytes. False if not mounted. */
bool sd_card_usage(uint64_t *total, uint64_t *used);

/**
 * Read up to len bytes into dst. Use this for big reads into PSRAM: ESP-IDF can't DMA into PSRAM
 * and would fall back to one 512-byte sector per command (slow), so reads go through an internal
 * DMA bounce buffer in multi-sector chunks. Internal-RAM destinations are read directly.
 */
size_t sd_read(fs::File &f, void *dst, size_t len);
