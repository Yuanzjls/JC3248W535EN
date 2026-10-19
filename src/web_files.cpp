#include "web_files.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform_time.h"
#include "sd_card.h"
#include "serial_log.h"

/*
 * File manager endpoints for the web page. Paths are card paths ("/mjpeg/clip.mjpeg"), passed
 * percent-encoded in the "path" query parameter. Writes use PUT / DELETE / JSON POST, which a
 * page from another site can't send through the user's browser (CORS preflight, never answered).
 */

static const size_t k_io_buf = 8 * 1024;       /* per transfer; internal DMA RAM: SD reads/writes direct */
static const size_t k_max_entries = 500;       /* listing cap for huge folders */
static const int k_recv_retries = 6;           /* x recv_wait_timeout (10 s) before giving up */
static const uint64_t k_space_margin = 64 * 1024; /* keep some room for FAT/directory growth */

/* ---------- small helpers ---------- */

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

static esp_err_t send_json(httpd_req_t *req, const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), (ssize_t)out.size());
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** Percent-decode (and '+' -> space) a query value. False on a malformed escape. */
static bool url_decode(const char *in, std::string *out) {
    out->clear();
    for (const char *p = in; *p; p++) {
        if (*p == '%') {
            const int hi = hex_digit(p[1]);
            const int lo = hi >= 0 ? hex_digit(p[2]) : -1;
            if (lo < 0) {
                return false;
            }
            *out += (char)((hi << 4) | lo);
            p += 2;
        } else {
            *out += (*p == '+') ? ' ' : *p;
        }
    }
    return true;
}

/**
 * A card path we're willing to touch: absolute, no "." / ".." components, no characters FAT
 * can't store, no control characters. "/" alone is the card root. Trailing '/' removed.
 */
static bool valid_path(const std::string &p) {
    if (p.empty() || p[0] != '/' || p.size() > 240) {
        return false;
    }
    size_t start = 1;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) {
            end = p.size();
        }
        const std::string comp = p.substr(start, end - start);
        if (comp == "." || comp == "..") {
            return false;
        }
        if (comp.empty() && end != p.size()) {
            return false; /* "//" */
        }
        for (unsigned char c : comp) {
            if (c < 0x20 || strchr("\\:*?\"<>|", c)) {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

/** Read and validate the "path" query parameter. Sends the error response itself on failure. */
static bool query_path(httpd_req_t *req, std::string *path, bool *flag = nullptr, const char *flag_key = nullptr) {
    char query[600];
    char raw[520];
    std::string p;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw, sizeof(raw)) != ESP_OK || !url_decode(raw, &p)) {
        send_text(req, "400 Bad Request", "path missing or too long");
        return false;
    }
    while (p.size() > 1 && p.back() == '/') {
        p.pop_back();
    }
    if (!valid_path(p)) {
        send_text(req, "400 Bad Request", "invalid path");
        return false;
    }
    if (flag) {
        char v[8];
        *flag = httpd_query_key_value(query, flag_key, v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
    }
    *path = p;
    return true;
}

static const char *base_name(const std::string &p) {
    const size_t slash = p.rfind('/');
    return p.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

static const char *mime_type(const char *name) {
    static const struct {
        const char *ext;
        const char *type;
    } k_types[] = {
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},   {".bmp", "image/bmp"},
        {".gif", "image/gif"},  {".txt", "text/plain; charset=utf-8"},            {".ini", "text/plain; charset=utf-8"},
        {".log", "text/plain; charset=utf-8"},          {".csv", "text/plain; charset=utf-8"},
        {".json", "application/json"},                   {".htm", "text/html; charset=utf-8"},
        {".html", "text/html; charset=utf-8"},           {".mp4", "video/mp4"},
    };
    const size_t n = strlen(name);
    for (const auto &t : k_types) {
        const size_t e = strlen(t.ext);
        if (n >= e && strcasecmp(name + n - e, t.ext) == 0) {
            return t.type;
        }
    }
    return "application/octet-stream";
}

/** RFC 5987 percent-encoding for Content-Disposition's filename*. */
static std::string rfc5987(const char *s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-._~", *p)) {
            out += (char)*p;
        } else {
            out += '%';
            out += hex[*p >> 4];
            out += hex[*p & 15];
        }
    }
    return out;
}

/** 8 KB transfer buffer: internal DMA-capable RAM (SD reads/writes go straight to it), else PSRAM. */
static uint8_t *io_alloc(void) {
    void *b = heap_caps_malloc(k_io_buf, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return (uint8_t *)(b ? b : heap_caps_malloc(k_io_buf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* ---------- handlers ---------- */

/** GET /api/files?path=/dir : folder listing + card space. */
static esp_err_t list_get(httpd_req_t *req) {
    std::string path;
    if (!query_path(req, &path)) {
        return ESP_OK;
    }
    const std::string full = sd_path(path.c_str());
    DIR *dir = opendir(full.c_str());
    if (!dir) {
        return send_text(req, "404 Not Found", "folder not found");
    }
    JsonDocument doc;
    doc["path"] = path;
    JsonArray entries = doc["entries"].to<JsonArray>();
    size_t n = 0;
    bool truncated = false;
    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name[0] == '.') {
            continue; /* hidden: video index cache, unfinished uploads, macOS "._" files */
        }
        if (n >= k_max_entries) {
            truncated = true;
            break;
        }
        struct stat st = {};
        (void)stat((full + "/" + de->d_name).c_str(), &st);
        JsonObject e = entries.add<JsonObject>();
        e["name"] = de->d_name;
        e["dir"] = S_ISDIR(st.st_mode);
        e["size"] = (uint64_t)st.st_size;
        e["mtime"] = (long long)st.st_mtime;
        n++;
    }
    closedir(dir);
    doc["truncated"] = truncated;
    uint64_t total = 0, used = 0;
    if (sd_card_usage(&total, &used)) {
        doc["total"] = total;
        doc["free"] = total - used;
    }
    return send_json(req, doc);
}

/** GET /api/file?path=/x[&download=1] : stream a file (shown in the browser, or saved). */
static esp_err_t file_get(httpd_req_t *req) {
    std::string path;
    bool download = false;
    if (!query_path(req, &path, &download, "download")) {
        return ESP_OK;
    }
    const int fd = sd_open_read(path.c_str());
    struct stat st = {};
    if (fd < 0 || fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (fd >= 0) {
            close(fd);
        }
        return send_text(req, "404 Not Found", "file not found");
    }
    uint8_t *buf = io_alloc();
    if (!buf) {
        close(fd);
        return send_text(req, "503 Service Unavailable", "out of memory");
    }
    const char *name = base_name(path);
    httpd_resp_set_type(req, mime_type(name));
    const std::string disp = std::string(download ? "attachment" : "inline") + "; filename*=UTF-8''" + rfc5987(name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const uint32_t t0 = now_ms();
    size_t sent = 0;
    bool ok = true;
    for (;;) {
        const size_t n = sd_read(fd, buf, k_io_buf);
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) {
            ok = false; /* client went away */
            break;
        }
        sent += n;
    }
    close(fd);
    heap_caps_free(buf);
    if (ok) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    const uint32_t ms = now_ms() - t0;
    serial_log("[web] sent %s: %u KB in %lu ms%s", path.c_str(), (unsigned)(sent / 1024), (unsigned long)ms,
               ok ? "" : " (aborted)");
    return ok ? ESP_OK : ESP_FAIL;
}

/**
 * PUT /api/file?path=/dir/name[&overwrite=1] : upload. The body is streamed to a hidden temporary
 * file in the target folder and renamed when complete, so an interrupted upload never leaves a
 * half file under the real name.
 */
static esp_err_t file_put(httpd_req_t *req) {
    std::string path;
    bool overwrite = false;
    if (!query_path(req, &path, &overwrite, "overwrite")) {
        return ESP_OK;
    }
    if (path == "/" || base_name(path)[0] == '.') {
        return send_text(req, "400 Bad Request", "invalid file name");
    }
    const std::string full = sd_path(path.c_str());
    struct stat st;
    const bool exists = stat(full.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        return send_text(req, "409 Conflict", "a folder with this name exists");
    }
    if (exists && !overwrite) {
        return send_text(req, "409 Conflict", "file exists");
    }
    const size_t parent_len = path.rfind('/');
    const std::string parent_full = sd_path(parent_len == 0 ? "/" : path.substr(0, parent_len).c_str());
    if (stat(parent_full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return send_text(req, "404 Not Found", "folder not found");
    }
    uint64_t total = 0, used = 0;
    if (sd_card_usage(&total, &used) && (uint64_t)req->content_len + k_space_margin > total - used) {
        return send_text(req, "507 Insufficient Storage", "not enough free space on the SD card");
    }

    char tmp_name[32];
    snprintf(tmp_name, sizeof(tmp_name), "/.upload-%08lx.part", (unsigned long)esp_random());
    const std::string tmp_full = parent_full + tmp_name;
    const int fd = open(tmp_full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    uint8_t *buf = fd >= 0 ? io_alloc() : nullptr;
    if (!buf) {
        if (fd >= 0) {
            close(fd);
            unlink(tmp_full.c_str());
        }
        return send_text(req, "500 Internal Server Error", fd < 0 ? "cannot create file" : "out of memory");
    }

    const uint32_t t0 = now_ms();
    size_t got = 0;
    int timeouts = 0;
    const char *fail = nullptr;
    while (got < req->content_len) {
        const size_t want = (req->content_len - got) < k_io_buf ? (req->content_len - got) : k_io_buf;
        const int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < k_recv_retries) {
            continue;
        }
        if (n <= 0) {
            fail = "connection lost";
            break;
        }
        timeouts = 0;
        if (write(fd, buf, (size_t)n) != n) {
            fail = "SD card write failed";
            break;
        }
        got += (size_t)n;
    }
    heap_caps_free(buf);
    if (close(fd) != 0 && !fail) {
        fail = "SD card write failed";
    }
    if (!fail && exists && unlink(full.c_str()) != 0) {
        fail = "cannot replace the existing file";
    }
    if (!fail && rename(tmp_full.c_str(), full.c_str()) != 0) {
        fail = "cannot rename the uploaded file";
    }
    if (fail) {
        unlink(tmp_full.c_str());
        serial_log("[web] upload %s failed after %u KB: %s", path.c_str(), (unsigned)(got / 1024), fail);
        return send_text(req, "500 Internal Server Error", fail);
    }
    const uint32_t ms = now_ms() - t0;
    serial_log("[web] received %s: %u KB in %lu ms (%lu KB/s)", path.c_str(), (unsigned)(got / 1024),
               (unsigned long)ms, (unsigned long)(ms ? got / ms : 0));
    return send_text(req, "200 OK", "OK");
}

/** DELETE /api/file?path=/x : a file, or an empty folder. */
static esp_err_t file_delete(httpd_req_t *req) {
    std::string path;
    if (!query_path(req, &path)) {
        return ESP_OK;
    }
    if (path == "/") {
        return send_text(req, "400 Bad Request", "cannot delete the card root");
    }
    const std::string full = sd_path(path.c_str());
    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
        return send_text(req, "404 Not Found", "not found");
    }
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(full.c_str()) != 0) {
            return send_text(req, "409 Conflict", "folder is not empty");
        }
    } else if (unlink(full.c_str()) != 0) {
        return send_text(req, "500 Internal Server Error", "delete failed");
    }
    serial_log("[web] deleted %s", path.c_str());
    return send_text(req, "200 OK", "OK");
}

/** POST /api/mkdir with JSON {"path": "/dir/new"}. */
static esp_err_t mkdir_post(httpd_req_t *req) {
    char type[48] = "";
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncmp(type, "application/json", 16) != 0) {
        return send_text(req, "415 Unsupported Media Type", "JSON only");
    }
    char body[600];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return send_text(req, "400 Bad Request", "body missing or too large");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    JsonDocument in;
    if (deserializeJson(in, body, got)) {
        return send_text(req, "400 Bad Request", "invalid JSON");
    }
    std::string path = in["path"] | "";
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (!valid_path(path) || path == "/" || base_name(path)[0] == '.') {
        return send_text(req, "400 Bad Request", "invalid folder name");
    }
    if (mkdir(sd_path(path.c_str()).c_str(), 0777) != 0) {
        return send_text(req, "409 Conflict", errno == EEXIST ? "already exists" : "cannot create folder");
    }
    serial_log("[web] created folder %s", path.c_str());
    return send_text(req, "200 OK", "OK");
}

void web_files_register(httpd_handle_t server) {
    static const httpd_uri_t k_routes[] = {
        {"/api/files", HTTP_GET, list_get, nullptr},
        {"/api/file", HTTP_GET, file_get, nullptr},
        {"/api/file", HTTP_PUT, file_put, nullptr},
        {"/api/file", HTTP_DELETE, file_delete, nullptr},
        {"/api/mkdir", HTTP_POST, mkdir_post, nullptr},
    };
    for (const httpd_uri_t &r : k_routes) {
        httpd_register_uri_handler(server, &r);
    }
}
