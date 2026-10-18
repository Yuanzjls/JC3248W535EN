#include "airthings.h"

#include <NimBLEDevice.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sd_text.h"

#include "air_ui.h"
#include "esp_bsp.h"
#include "serial_log.h"
#include "platform_time.h"

/*
 * Airthings Wave sensors over BLE (NimBLE). No pairing: the readings are a plain GATT read.
 *  - Find: scan 10 s; Airthings advertises manufacturer data with company ID 0x0334 followed by
 *    the 32-bit serial number (little endian). The serial's first 4 digits give the model.
 *  - Read: connect to the saved address, read one ~20 byte characteristic, disconnect (~1 s).
 * Formats follow Airthings' open-source readers (wave-reader / waveplus-reader). Every read logs
 * the raw bytes so the decoding can be checked against the Airthings app.
 */

static const uint16_t k_company_id = 0x0334;
static const uint32_t k_period_ms = 5UL * 60UL * 1000UL;
static const uint32_t k_scan_ms = 10000;
static const char *k_ini = "/airthings.ini";

/* Wave Radon / Wave 2nd gen (model 2950) */
static const char *k_wave2_svc = "b42e4a8e-ade7-11e4-89d3-123b93f75cba";
static const char *k_wave2_chr = "b42e4dcc-ade7-11e4-89d3-123b93f75cba";
/* Wave Plus (model 2930) */
static const char *k_plus_svc = "b42e1c08-ade7-11e4-89d3-123b93f75cba";
static const char *k_plus_chr = "b42e2a68-ade7-11e4-89d3-123b93f75cba";

struct Config {
    uint32_t serial;
    std::string address;
    uint8_t addr_type;
};

static Config s_cfg = {0, "", 0};
static AirData s_data = {};
static std::vector<AirFound> s_found;
static char s_status[80] = "";
static TaskHandle_t s_task = nullptr;
static volatile bool s_scan_req = false;
static volatile bool s_busy = false;
static bool s_ble_on = false;

/* ---------- helpers ---------- */

static uint32_t model_of(uint32_t serial) {
    return serial / 1000000UL; /* 2950123456 -> 2950 */
}

const char *airthings_model_name(uint32_t serial) {
    switch (model_of(serial)) {
    case 2900: return "Wave (1st gen)";
    case 2920: return "Wave Mini";
    case 2930: return "Wave Plus";
    case 2950: return "Wave Radon";
    default: return "Airthings";
    }
}

static uint16_t u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/** Set the status line and tell the UI (task side). */
static void set_status(const char *fmt, ...) {
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_log("[air] %s", buf[0] ? buf : "OK");
    bsp_display_lock(0);
    strlcpy(s_status, buf, sizeof(s_status));
    air_ui_on_update();
    bsp_display_unlock();
}

/* ---------- config on SD ---------- */

static void load_config(void) {
    std::string content;
    if (!sd_read_text(k_ini, &content)) {
        serial_log("[air] %s not found: no sensor chosen yet", k_ini);
        return;
    }
    ini_parse(content, [](const std::string &, const std::string &key, const std::string &val) {
        if (key == "serial") {
            s_cfg.serial = strtoul(val.c_str(), nullptr, 10);
        } else if (key == "address") {
            s_cfg.address = val;
        } else if (key == "type") {
            s_cfg.addr_type = (uint8_t)atoi(val.c_str());
        }
    });
    serial_log("[air] sensor: %s SN %lu at %s", airthings_model_name(s_cfg.serial), (unsigned long)s_cfg.serial,
               s_cfg.address.c_str());
}

static void save_config(void) {
    char text[128];
    snprintf(text, sizeof(text), "[airthings]\nserial=%lu\naddress=%s\ntype=%u\n", (unsigned long)s_cfg.serial,
             s_cfg.address.c_str(), (unsigned)s_cfg.addr_type);
    if (!sd_write_text(k_ini, text)) {
        serial_log("[air] cannot write %s", k_ini);
    }
}

/* ---------- BLE ---------- */

static void ensure_ble(void) {
    if (s_ble_on) {
        return;
    }
    const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    NimBLEDevice::init("");
    s_ble_on = true;
    serial_log("[air] BLE started (internal RAM used: %lu KB)",
               (unsigned long)((before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
}

static void do_scan(void) {
    ensure_ble();
    set_status("Searching for Airthings sensors...");
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults res = scan->getResults(k_scan_ms, false);

    std::vector<AirFound> found;
    for (int i = 0; i < res.getCount(); i++) {
        const NimBLEAdvertisedDevice *d = res.getDevice(i);
        for (uint8_t m = 0; m < d->getManufacturerDataCount(); m++) {
            const std::string md = d->getManufacturerData(m);
            if (md.size() < 6 || u16le((const uint8_t *)md.data()) != k_company_id) {
                continue;
            }
            const uint8_t *p = (const uint8_t *)md.data() + 2;
            const uint32_t serial = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
            found.push_back({serial, d->getAddress().toString(), d->getAddress().getType(),
                             d->getRSSI()});
            serial_log("[air] found %s SN %lu at %s, %d dBm", airthings_model_name(serial), (unsigned long)serial,
                       d->getAddress().toString().c_str(), d->getRSSI());
        }
    }
    serial_log("[air] scan: %d BLE device(s), %u Airthings", res.getCount(), (unsigned)found.size());
    scan->clearResults();

    bsp_display_lock(0);
    s_found = found;
    bsp_display_unlock();
    set_status(found.empty() ? "No Airthings sensor found - is it within ~10 m?" : "");
}

/** Log every service/characteristic: for a model or firmware whose layout we don't know. */
static void dump_attributes(NimBLEClient *c) {
    for (NimBLERemoteService *s : c->getServices(true)) {
        serial_log("[air]   service %s", s->getUUID().toString().c_str());
        for (NimBLERemoteCharacteristic *ch : s->getCharacteristics(true)) {
            serial_log("[air]     characteristic %s", ch->getUUID().toString().c_str());
        }
    }
}

static void decode(const uint8_t *raw, size_t len, uint32_t model, AirData *out) {
    /* Both layouts: version, humidity*2, two bytes, then little-endian u16s:
     *   Wave Radon (2950): radon 24h, radon long-term, temperature*100
     *   Wave Plus  (2930): radon 24h, radon long-term, temperature*100, pressure*50, CO2, VOC */
    out->humidity = raw[1] / 2.0f;
    const uint16_t st = u16le(raw + 4), lt = u16le(raw + 6);
    out->radon_st = st > 16383 ? -1 : st; /* out-of-range = "no value yet" */
    out->radon_lt = lt > 16383 ? -1 : lt;
    out->temp_c = u16le(raw + 8) / 100.0f;
    out->has_plus = model == 2930 && len >= 16;
    if (out->has_plus) {
        out->pressure_hpa = u16le(raw + 10) / 50.0f;
        out->co2_ppm = u16le(raw + 12);
        out->voc_ppb = u16le(raw + 14);
    }
}

static void do_read(void) {
    Config cfg;
    bsp_display_lock(0);
    cfg = s_cfg;
    bsp_display_unlock();
    if (cfg.serial == 0 || cfg.address.length() == 0) {
        return;
    }
    const uint32_t model = model_of(cfg.serial);
    if (model != 2950 && model != 2930) {
        set_status("%s has no radon data this app can read", airthings_model_name(cfg.serial));
        return;
    }
    ensure_ble();
    set_status("Reading sensor...");
    const uint32_t t0 = now_ms();

    NimBLEClient *c = NimBLEDevice::createClient();
    c->setConnectTimeout(8000);
    if (!c->connect(NimBLEAddress(cfg.address, cfg.addr_type))) {
        NimBLEDevice::deleteClient(c);
        set_status("Can't connect - sensor busy (phone app?) or out of range");
        return;
    }
    const int rssi = c->getRssi();
    NimBLERemoteCharacteristic *ch = nullptr;
    NimBLERemoteService *svc = c->getService(model == 2950 ? k_wave2_svc : k_plus_svc);
    if (svc) {
        ch = svc->getCharacteristic(model == 2950 ? k_wave2_chr : k_plus_chr);
    }
    uint8_t raw[32];
    size_t len = 0;
    if (ch) {
        const NimBLEAttValue v = ch->readValue();
        len = v.size() < sizeof(raw) ? v.size() : sizeof(raw);
        memcpy(raw, v.data(), len);
    } else {
        serial_log("[air] expected characteristic not found; the sensor offers:");
        dump_attributes(c);
    }
    c->disconnect();
    NimBLEDevice::deleteClient(c);

    if (!ch) {
        set_status("Unknown data format (attributes listed in the serial log)");
        return;
    }
    char hex[3 * sizeof(raw) + 1];
    for (size_t i = 0; i < len; i++) {
        snprintf(hex + 3 * i, 4, "%02x ", raw[i]);
    }
    hex[len ? 3 * len - 1 : 0] = '\0';
    serial_log("[air] raw (%u bytes): %s", (unsigned)len, hex);
    if (len < 10 || raw[0] != 1) {
        set_status("Unexpected data (version %u, %u bytes) - see serial log", len ? raw[0] : 0, (unsigned)len);
        return;
    }

    AirData d = {};
    decode(raw, len, model, &d);
    d.valid = true;
    d.serial = cfg.serial;
    d.rssi = rssi;
    const time_t now = time(nullptr);
    d.updated = now > 1700000000 ? now : 0;
    serial_log("[air] radon 24h %d, long-term %d Bq/m3 | %.2f C | %.1f %% RH | %d dBm | %lu ms", d.radon_st,
               d.radon_lt, d.temp_c, d.humidity, rssi, (unsigned long)(now_ms() - t0));
    bsp_display_lock(0);
    s_data = d;
    bsp_display_unlock();
    set_status("");
}

static void air_task(void *arg) {
    LV_UNUSED(arg);
    /* First read 10 s after boot: let WiFi/NTP settle (they share the radio). */
    uint32_t wait = pdMS_TO_TICKS(10000);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait);
        s_busy = true;
        if (s_scan_req) {
            s_scan_req = false;
            do_scan();
        } else {
            do_read();
        }
        s_busy = false;
        bsp_display_lock(0);
        air_ui_on_update();
        bsp_display_unlock();
        wait = airthings_configured() ? pdMS_TO_TICKS(k_period_ms) : portMAX_DELAY;
    }
}

/* ---------- API ---------- */

void airthings_begin(void) {
    if (s_task) {
        return;
    }
    load_config();
    /* Core 1, low priority: BLE calls block while connecting; the NimBLE host runs on its own task. */
    /* Stack must stay in INTERNAL RAM: NimBLEDevice::init() reads its pairing store from NVS (flash),
     * and flash access disables the cache that PSRAM is reached through - a PSRAM stack asserts
     * (esp_task_stack_is_sane_cache_disabled) and reboots. */
    xTaskCreatePinnedToCore(air_task, "airthings", 6144, nullptr, 1, &s_task, 1);
}

void airthings_find(void) {
    s_scan_req = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void airthings_read_now(void) {
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void airthings_choose(size_t i) {
    if (i >= s_found.size()) {
        return;
    }
    s_cfg.serial = s_found[i].serial;
    s_cfg.address = s_found[i].address;
    s_cfg.addr_type = s_found[i].addr_type;
    s_data = {};
    save_config();
    serial_log("[air] chose %s SN %lu", airthings_model_name(s_cfg.serial), (unsigned long)s_cfg.serial);
    airthings_read_now();
}

bool airthings_configured(void) {
    return s_cfg.serial != 0 && s_cfg.address.length() > 0;
}

uint32_t airthings_serial(void) {
    return s_cfg.serial;
}

bool airthings_busy(void) {
    return s_busy;
}

const AirData &airthings_data(void) {
    return s_data;
}

const std::vector<AirFound> &airthings_found(void) {
    return s_found;
}

const char *airthings_status(void) {
    return s_status;
}
