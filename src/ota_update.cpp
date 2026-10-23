#include "ota_update.h"

#include <ArduinoJson.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <algorithm>

#include "platform_time.h"
#include "serial_log.h"

/*
 * Upload: PUT /api/ota with firmware.bin as the body (from the web page). The handler hands the
 * request to a task with an internal-RAM stack (httpd_req_async_handler_begin): writing flash turns
 * the cache off, and the web server's own task has its stack in PSRAM, which is unreachable then.
 * The image is streamed into the inactive app slot in 4 KB pieces - nothing is buffered whole.
 * Before the first byte is written, the image header must be an ESP32-S3 app of this project
 * (esp_app_desc_t.project_name), so another board's firmware or a random file is refused; at the
 * end esp_ota_end() verifies the checksum / SHA-256, and only then the boot slot is switched.
 *
 * Rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): the bootloader starts a new image as
 * PENDING_VERIFY. ota_boot() confirms it after k_confirm_s of normal running; if the board resets
 * before that - crash, watchdog, the UI watchdog's restart, power - the bootloader marks it invalid
 * and boots the previous firmware.
 */

static const size_t k_chunk = 4096;
static const uint32_t k_confirm_s = 60;
static const int k_recv_retries = 6;          /* x the server's recv_wait_timeout (10 s) */
static const uint64_t k_restart_delay_us = 1500 * 1000; /* let the response reach the browser */

/* Image start: header, first segment header, then the app description (esp_app_format). */
static const size_t k_desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
static const size_t k_head_len = k_desc_off + sizeof(esp_app_desc_t);

static volatile bool s_busy = false;
static bool s_pending = false;          /* running image still on probation */
static std::string s_rolled_back;       /* version of an update that was rolled back, if any */

/* ---------- boot ---------- */

const char *ota_version(void) {
    return esp_app_get_description()->version;
}

const char *ota_slot(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

static void confirm_cb(void *arg) {
    (void)arg;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    s_pending = err != ESP_OK;
    serial_log("[ota] firmware %s confirmed after %lu s: %s", ota_version(), (unsigned long)k_confirm_s,
               esp_err_to_name(err));
}

void ota_boot(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    (void)esp_ota_get_state_partition(run, &state);
    serial_log("[ota] running %s from %s (built %s %s)", ota_version(), run->label, esp_app_get_description()->date,
               esp_app_get_description()->time);

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bad_desc;
    if (bad && esp_ota_get_partition_description(bad, &bad_desc) == ESP_OK) {
        s_rolled_back = bad_desc.version;
        serial_log("[ota] update %s in %s did not start properly - rolled back", bad_desc.version, bad->label);
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        serial_log("[ota] new firmware: confirming after %lu s of normal running", (unsigned long)k_confirm_s);
        const esp_timer_create_args_t args = {confirm_cb, nullptr, ESP_TIMER_TASK, "ota_ok", false};
        esp_timer_handle_t t;
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, (uint64_t)k_confirm_s * 1000000ULL);
        }
    }
}

/* ---------- web ---------- */

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

static esp_err_t info_get(httpd_req_t *req) {
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    JsonDocument doc;
    doc["version"] = d->version;
    doc["built"] = std::string(d->date) + " " + d->time;
    doc["idf"] = d->idf_ver;
    doc["slot"] = ota_slot();
    doc["next_slot"] = next ? next->label : "";
    doc["max_size"] = next ? next->size : 0;
    doc["pending"] = s_pending;
    doc["rolled_back"] = s_rolled_back;
    doc["busy"] = (bool)s_busy;
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), out.size());
}

/** Is this the start of an ESP32-S3 app image of this project? nullptr if so, else why not. */
static const char *check_image(const uint8_t *head) {
    const esp_image_header_t *h = (const esp_image_header_t *)head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(head + k_desc_off);
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not an ESP32 firmware image (use .pio/build/JC3248W535EN/firmware.bin)";
    }
    if (h->chip_id != ESP_CHIP_ID_ESP32S3) {
        return "firmware is for a different chip";
    }
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof(d->project_name)) != 0) {
        return "firmware is for a different project";
    }
    return nullptr;
}

static void restart_cb(void *arg) {
    (void)arg;
    esp_restart();
}

/** Receive the image and install it (runs in ota_task). Returns the reply; *ok on success. */
static const char *receive_and_install(httpd_req_t *req, bool *ok, std::string *msg) {
    *ok = false;
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        return "no update slot in the partition table";
    }
    if (req->content_len < k_head_len || req->content_len > part->size) {
        *msg = "file size " + std::to_string(req->content_len) + " B; the slot takes up to " +
               std::to_string(part->size) + " B";
        return msg->c_str();
    }
    /* Internal RAM: the source of a flash write must be readable while the cache is off. */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(k_chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        return "out of memory";
    }

    const uint32_t t0 = now_ms();
    esp_ota_handle_t h = 0;
    bool started = false;
    const char *fail = nullptr;
    size_t got = 0, fill = 0, next_log = 512 * 1024;
    int timeouts = 0;
    while (got + fill < req->content_len && !fail) {
        const size_t want = std::min(k_chunk - fill, req->content_len - got - fill);
        const int n = httpd_req_recv(req, (char *)buf + fill, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < k_recv_retries) {
            continue;
        }
        if (n <= 0) {
            fail = "connection lost";
            break;
        }
        timeouts = 0;
        fill += (size_t)n;
        const bool last = got + fill >= req->content_len;
        if (fill < k_chunk && !last) {
            continue; /* write whole 4 KB pieces */
        }
        if (!started) {
            if ((fail = check_image(buf)) != nullptr) {
                break;
            }
            const esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err != ESP_OK) {
                *msg = std::string("cannot start the update: ") + esp_err_to_name(err);
                fail = msg->c_str();
                break;
            }
            started = true;
            serial_log("[ota] receiving %u KB into %s", (unsigned)(req->content_len / 1024), part->label);
        }
        const esp_err_t err = esp_ota_write(h, buf, fill);
        if (err != ESP_OK) {
            *msg = std::string("flash write failed: ") + esp_err_to_name(err);
            fail = msg->c_str();
            break;
        }
        got += fill;
        fill = 0;
        if (got >= next_log) {
            serial_log("[ota] %u / %u KB", (unsigned)(got / 1024), (unsigned)(req->content_len / 1024));
            next_log += 512 * 1024;
        }
    }
    heap_caps_free(buf);

    if (!fail && started) {
        const esp_err_t err = esp_ota_end(h); /* checks the image (checksum, SHA-256) */
        started = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            fail = "the image is damaged (verification failed)";
        } else if (err != ESP_OK) {
            *msg = std::string("cannot finish the update: ") + esp_err_to_name(err);
            fail = msg->c_str();
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            fail = "cannot select the new firmware";
        }
    }
    if (started) {
        esp_ota_abort(h);
    }
    if (fail) {
        serial_log("[ota] update failed after %u KB: %s", (unsigned)(got / 1024), fail);
        return fail;
    }
    serial_log("[ota] installed %u KB in %s in %lu ms - restarting", (unsigned)(got / 1024), part->label,
               (unsigned long)(now_ms() - t0));
    *ok = true;
    return "installed - restarting";
}

static void ota_task(void *arg) {
    httpd_req_t *req = (httpd_req_t *)arg;
    bool ok = false;
    std::string msg;
    const char *reply = receive_and_install(req, &ok, &msg);
    send_text(req, ok ? "200 OK" : "400 Bad Request", reply);
    httpd_req_async_handler_complete(req);
    if (ok) {
        const esp_timer_create_args_t args = {restart_cb, nullptr, ESP_TIMER_TASK, "ota_restart", false};
        esp_timer_handle_t t;
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, k_restart_delay_us);
        }
    } else {
        s_busy = false;
    }
    vTaskDelete(nullptr);
}

static esp_err_t upload_put(httpd_req_t *req) {
    if (s_busy) {
        return send_text(req, "409 Conflict", "an update is already running");
    }
    httpd_req_t *async = nullptr;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        return send_text(req, "500 Internal Server Error", "cannot start the update");
    }
    s_busy = true;
    /* Internal-RAM stack (xTaskCreate): flash writes with the cache off. */
    if (xTaskCreatePinnedToCore(ota_task, "ota", 6144, async, 3, nullptr, 1) != pdPASS) {
        s_busy = false;
        send_text(async, "500 Internal Server Error", "out of memory");
        httpd_req_async_handler_complete(async);
    }
    return ESP_OK;
}

void ota_web_register(httpd_handle_t server) {
    static const httpd_uri_t k_routes[] = {
        {"/api/ota", HTTP_GET, info_get, nullptr},
        {"/api/ota", HTTP_PUT, upload_put, nullptr},
    };
    for (const httpd_uri_t &r : k_routes) {
        httpd_register_uri_handler(server, &r);
    }
}
