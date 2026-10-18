#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

/*
 * SD card through ESP-IDF's own drivers + FATFS at "/sd", instead of the Arduino SD library:
 *  - preferred: native SD bus, 1-bit (SDMMC peripheral) - hardware handles waits, CRC and DMA,
 *    one interrupt per transfer, no polling;
 *  - fallback: SPI mode (sdspi on SPI3) - DMA data, but the wait for each block is polled. Used
 *    when the card is still in SPI mode (only a power cycle switches it back).
 * The Arduino library busy-waited on every byte (core 0 at ~90 % during video, a watchdog reset,
 * a starved UI).
 *
 * File access is plain POSIX on the mount point (open/read/lseek/close, opendir/readdir, stat,
 * mkdir, fopen for small files - see sd_text.h); sd_path() turns a card path into a full one.
 */

/** Mount at "/sd" (call once at boot). Recovers a card left mid-transfer by a reset. */
bool sd_card_mount(void);

bool sd_card_mounted(void);

/** Full path for a card path: "/settings.ini" -> "/sd/settings.ini". */
std::string sd_path(const char *path);

/** open() a card path read-only. File descriptor, or -1. */
int sd_open_read(const char *path);

/** Size of an open file in bytes (0 on error). */
size_t sd_file_size(int fd);

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
size_t sd_read(int fd, void *dst, size_t len);
