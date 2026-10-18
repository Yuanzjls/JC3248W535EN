#include "sd_text.h"

#include <stdio.h>
#include <strings.h>

#include "sd_card.h"

bool sd_read_text(const char *path, std::string *out) {
    out->clear();
    FILE *f = fopen(sd_path(path).c_str(), "rb");
    if (!f) {
        return false;
    }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out->append(buf, n);
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool sd_write_text(const char *path, const std::string &text) {
    FILE *f = fopen(sd_path(path).c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return (fclose(f) == 0) && ok;
}

std::string str_trim(const std::string &s) {
    static const char *ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return std::string();
    }
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool str_ieq(const std::string &a, const char *b) {
    return strcasecmp(a.c_str(), b) == 0;
}

void for_each_line(const std::string &text, const std::function<void(const std::string &line)> &fn) {
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        fn(line);
        start = nl + 1;
    }
}

void ini_parse(const std::string &text,
               const std::function<void(const std::string &section, const std::string &key,
                                        const std::string &value)> &fn) {
    std::string section;
    for_each_line(text, [&](const std::string &raw) {
        std::string line = raw;
        const size_t semi = line.find(';');
        if (semi != std::string::npos) {
            line.erase(semi);
        }
        line = str_trim(line);
        if (line.empty()) {
            return;
        }
        if (line.front() == '[') {
            const size_t close = line.find(']');
            section = str_trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            return;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            return;
        }
        fn(section, str_trim(line.substr(0, eq)), str_trim(line.substr(eq + 1)));
    });
}
