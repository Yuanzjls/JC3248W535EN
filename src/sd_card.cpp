#include "sd_card.h"

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_vfs_fat.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sd_protocol_defs.h>
#include <sdmmc_cmd.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "serial_log.h"
#include "platform_time.h"

/* Board wiring (JC3248W535EN): SD card on its own SPI bus. The display uses SPI2. */
static const spi_host_device_t k_host = SPI3_HOST;
static const gpio_num_t k_pin_cs = GPIO_NUM_10;
static const gpio_num_t k_pin_mosi = GPIO_NUM_11;
static const gpio_num_t k_pin_sck = GPIO_NUM_12;
static const gpio_num_t k_pin_miso = GPIO_NUM_13;
static const char *k_mount = "/sd"; /* must match lv_conf.h LV_FS_POSIX_PATH */

/* Bounce buffer for PSRAM reads: 8 KB = 16 sectors per multi-sector DMA read. */
static const size_t k_bounce_size = 8 * 1024;

static bool s_bus_ok = false;
static sdmmc_card_t *s_card = nullptr;
static uint8_t *s_bounce = nullptr;
static SemaphoreHandle_t s_bounce_lock = nullptr;
static const char *s_mode = "not mounted";

std::string sd_path(const char *path) {
    return std::string(k_mount) + (path[0] == '/' ? "" : "/") + path;
}

int sd_open_read(const char *path) {
    return open(sd_path(path).c_str(), O_RDONLY);
}

size_t sd_file_size(int fd) {
    struct stat st;
    return (fd >= 0 && fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
}

/*
 * A chip reset (USB reset from uploading or opening the monitor, watchdog, crash, reset button)
 * doesn't reset the SD card - it keeps its power. If the reset hit in the middle of:
 *  - a read: the card is still sending that block and ignores the init command;
 *  - a write: the card is busy programming its flash and holds its data line (MISO) low until done.
 * Either way the mount fails. So: clock out enough dummy bytes for any block in flight to finish,
 * wait (card selected) until it stops signalling busy, then deselect it and give it the 74+ idle
 * clocks that SD init expects. Uses a temporary raw SPI device at 400 kHz with CS driven by hand.
 */
static void sd_unstick(void) {
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 400000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    spi_device_handle_t h = nullptr;
    if (spi_bus_add_device(k_host, &dev, &h) != ESP_OK) {
        return;
    }
    gpio_reset_pin(k_pin_cs);
    gpio_set_direction(k_pin_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(k_pin_cs, 0);

    static uint8_t ff[64];
    static uint8_t rx[64];
    memset(ff, 0xFF, sizeof(ff));
    spi_transaction_t t = {};
    t.length = sizeof(ff) * 8;
    t.tx_buffer = ff;
    t.rx_buffer = rx;
    for (int i = 0; i < 1024 / (int)sizeof(ff); i++) { /* > one 512-byte block + CRC + token */
        spi_device_polling_transmit(h, &t);
    }

    /* Busy = card drives 0x00; idle = 0xFF. Wait for 8 idle bytes in a row (up to 500 ms). */
    spi_transaction_t b = {};
    b.length = 8;
    b.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    b.tx_data[0] = 0xFF;
    const uint32_t t0 = now_ms();
    int idle = 0;
    while (idle < 8 && now_ms() - t0 < 500) {
        spi_device_polling_transmit(h, &b);
        idle = (b.rx_data[0] == 0xFF) ? idle + 1 : 0;
    }
    if (idle < 8) {
        serial_log("[sd] card still busy after 500 ms");
    }

    gpio_set_level(k_pin_cs, 1);
    spi_transaction_t c = {}; /* fresh: the driver writes rxlength back into a used transaction */
    c.length = 16 * 8;        /* 128 clocks with the card deselected */
    c.tx_buffer = ff;
    c.rx_buffer = rx;
    spi_device_polling_transmit(h, &c);
    spi_bus_remove_device(h);
}

/*
 * ESP-IDF's card init always reads the SD Status Register (ACMD13) and fails the whole mount if
 * the card doesn't answer. This board's older 512 MB SDSC card doesn't answer it in SPI mode (the
 * Arduino SD library never asked). The SSR is only used for erase/discard, which we never do, so
 * a linker wrap (-Wl,--wrap=sdmmc_init_sd_ssr in platformio.ini) lets init continue without it.
 */
extern "C" esp_err_t __real_sdmmc_init_sd_ssr(sdmmc_card_t *card);
extern "C" esp_err_t __wrap_sdmmc_init_sd_ssr(sdmmc_card_t *card) {
    const esp_err_t err = __real_sdmmc_init_sd_ssr(card);
    if (err != ESP_OK) {
        serial_log("[sd] card didn't return its SD status register (%s) - continuing without it "
                   "(only used for erase/discard)",
                   esp_err_to_name(err));
        return ESP_OK;
    }
    return err;
}

static esp_vfs_fat_sdmmc_mount_config_t mount_config(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false; /* never wipe the user's card */
    mount.max_files = 8;                  /* video (stream + index scan + cache) + web upload/download + settings */
    mount.allocation_unit_size = 16 * 1024;
    return mount;
}

/*
 * Native SD bus, 1-bit (SDMMC peripheral): the hardware waits for the card's data, checks CRCs and
 * runs DMA, then raises one interrupt per transfer - no polling at all, unlike SPI mode.
 * The board wires CLK=12, CMD=11, DAT0=13 and DAT3=10; DAT1/DAT2 only go to pull-ups, so 4-bit
 * isn't possible. DAT3 must be high at CMD0 or the card chooses SPI mode.
 *
 * A card that was put in SPI mode stays there until its power is cut, so after running the SPI
 * firmware (or a reset that didn't power-cycle the card) this fails and we fall back to SPI.
 */
static bool try_sdmmc(void) {
    gpio_reset_pin(k_pin_cs);
    gpio_set_direction(k_pin_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(k_pin_cs, 1); /* DAT3 high: SD (not SPI) mode */

    const esp_vfs_fat_sdmmc_mount_config_t mount = mount_config();
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            sleep_ms(200);
        }
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz */
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = k_pin_sck;
        slot.cmd = k_pin_mosi;
        slot.d0 = k_pin_miso;
        slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
        slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
        slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP; /* in addition to the board's 10 k pull-ups */
        err = esp_vfs_fat_sdmmc_mount(k_mount, &host, &slot, &mount, &s_card);
        if (err == ESP_OK) {
            return true;
        }
        s_card = nullptr;
    }
    serial_log("[sd] SD bus (1-bit) mode not available (%s) - falling back to SPI mode", esp_err_to_name(err));
    return false;
}

/* SPI mode (sdspi on SPI3), with recovery for a card left mid-transfer by a reset. */
static bool try_sdspi(int *freq_khz_out) {
    if (!s_bus_ok) {
        /* SD expects pull-ups on its lines (especially the card's data out); cheap insurance. */
        gpio_set_pull_mode(k_pin_miso, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(k_pin_mosi, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(k_pin_cs, GPIO_PULLUP_ONLY);
        spi_bus_config_t bus = {};
        bus.mosi_io_num = k_pin_mosi;
        bus.miso_io_num = k_pin_miso;
        bus.sclk_io_num = k_pin_sck;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = k_bounce_size + 64;
        const esp_err_t err = spi_bus_initialize(k_host, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            serial_log("[sd] SPI bus init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_bus_ok = true;
    }
    const esp_vfs_fat_sdmmc_mount_config_t mount = mount_config();

    /*
     * 20 MHz first. Retries (after a reset that interrupted the card) run at 4 MHz with growing
     * pauses, ~2 s in total, before giving up.
     */
    static const int k_freq_khz[] = {20000, 4000, 4000, 4000, 4000};
    static const uint32_t k_pause_ms[] = {0, 100, 250, 500, 1000};
    static const int k_attempts = sizeof(k_freq_khz) / sizeof(k_freq_khz[0]);
    esp_err_t err = ESP_FAIL;
    int attempt = 0;
    for (; attempt < k_attempts; attempt++) {
        if (attempt > 0) {
            serial_log("[sd] mount failed (%s), recovering card (attempt %d/%d)", esp_err_to_name(err), attempt + 1,
                       k_attempts);
            sd_unstick();
            sleep_ms(k_pause_ms[attempt]);
        }
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = k_host;
        host.max_freq_khz = k_freq_khz[attempt];
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.gpio_cs = k_pin_cs;
        slot.host_id = k_host;
        err = esp_vfs_fat_sdspi_mount(k_mount, &host, &slot, &mount, &s_card);
        if (err == ESP_OK) {
            break;
        }
        s_card = nullptr;
    }
    if (err != ESP_OK) {
        serial_log("[sd] mount FAILED (%s) - card missing or stuck: unplug the board for a few seconds "
                   "to power-cycle the card",
                   esp_err_to_name(err));
        return false;
    }
    if (attempt > 0) {
        serial_log("[sd] recovered on attempt %d (at %d kHz)", attempt + 1, k_freq_khz[attempt]);
    }
    *freq_khz_out = k_freq_khz[attempt];
    return true;
}

bool sd_card_mount(void) {
    if (s_card) {
        return true;
    }
    if (!s_bounce) {
        s_bounce = (uint8_t *)heap_caps_malloc(k_bounce_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_bounce_lock = xSemaphoreCreateMutex();
    }

    int freq_khz = SDMMC_FREQ_DEFAULT;
    if (try_sdmmc()) {
        s_mode = "SD bus 1-bit, hardware DMA";
    } else if (try_sdspi(&freq_khz)) {
        s_mode = "SPI mode, DMA data";
    } else {
        return false;
    }
    serial_log("[sd] mount OK at \"%s\" (%s), %s %s, %llu MiB, %d kHz", k_mount, s_mode, s_card->cid.name,
               sd_card_type(), (unsigned long long)(sd_card_size_bytes() / (1024 * 1024)),
               s_card->max_freq_khz ? s_card->max_freq_khz : freq_khz);
    return true;
}

bool sd_card_mounted(void) {
    return s_card != nullptr;
}

const char *sd_card_type(void) {
    if (!s_card) {
        return "none";
    }
    if (s_card->is_mmc) {
        return "MMC";
    }
    return (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC" : "SDSC";
}

uint64_t sd_card_size_bytes(void) {
    return s_card ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size : 0;
}

bool sd_card_usage(uint64_t *total, uint64_t *used) {
    uint64_t t = 0, f = 0;
    if (!s_card || esp_vfs_fat_info(k_mount, &t, &f) != ESP_OK) {
        return false;
    }
    *total = t;
    *used = t - f;
    return true;
}

size_t sd_read(int fd, void *dst, size_t len) {
    if (esp_ptr_dma_capable(dst) || !s_bounce) {
        /* internal DMA-capable: FATFS reads straight into it */
        const ssize_t n = read(fd, dst, len);
        return n > 0 ? (size_t)n : 0;
    }
    size_t got = 0;
    xSemaphoreTake(s_bounce_lock, portMAX_DELAY);
    while (got < len) {
        const size_t want = (len - got) < k_bounce_size ? (len - got) : k_bounce_size;
        const ssize_t r = read(fd, s_bounce, want);
        if (r <= 0) {
            break;
        }
        const size_t n = (size_t)r;
        memcpy((uint8_t *)dst + got, s_bounce, n);
        got += n;
        if (n < want) {
            break; /* end of file */
        }
    }
    xSemaphoreGive(s_bounce_lock);
    return got;
}
