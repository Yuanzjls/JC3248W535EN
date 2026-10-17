#pragma once

#include <Arduino.h>
#include <vector>

/** A sensor seen during "Find sensor". */
struct AirFound {
    uint32_t serial; /* e.g. 2950123456; first 4 digits = model */
    String address;  /* BLE address "aa:bb:cc:dd:ee:ff" */
    uint8_t addr_type;
    int rssi;
};

/** Latest reading. Radon values are -1 when the sensor has none yet (first ~24 h). */
struct AirData {
    bool valid;
    uint32_t serial;
    int radon_st; /* 24 h average, Bq/m3 */
    int radon_lt; /* long-term average, Bq/m3 */
    float temp_c;
    float humidity;
    bool has_plus; /* Wave Plus extras below are valid */
    float pressure_hpa;
    int co2_ppm;
    int voc_ppb;
    int rssi;
    time_t updated;
};

/** Start the background task: reads the saved sensor every 5 min. Call once after SD mount. */
void airthings_begin(void);

/** Search for Airthings sensors for 10 s (results via airthings_found()). Any context. */
void airthings_find(void);

/** Read the saved sensor now. Any context. */
void airthings_read_now(void);

/** Use the i-th found sensor: saved to /airthings.ini, then read. LVGL context. */
void airthings_choose(size_t i);

bool airthings_configured(void);
uint32_t airthings_serial(void);
bool airthings_busy(void); /* scanning or reading */

/* LVGL context (the task updates these under bsp_display_lock). */
const AirData &airthings_data(void);
const std::vector<AirFound> &airthings_found(void);
const char *airthings_status(void);

/** "Wave Radon", "Wave Plus", ... from the serial number's model prefix. */
const char *airthings_model_name(uint32_t serial);
