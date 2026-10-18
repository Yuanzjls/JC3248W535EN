#pragma once

/*
 * Small text files on the SD card (settings, timezone, Wi-Fi list, sensor config) with POSIX
 * stdio and std::string - no Arduino File/String. Paths are card-relative ("/settings.ini");
 * the card is mounted at "/sd" (sd_card.cpp).
 */

#include <functional>
#include <string>

/** Read the whole file into *out. False if it doesn't exist or can't be read. */
bool sd_read_text(const char *path, std::string *out);

/** Replace the file's contents with text. False on any error. */
bool sd_write_text(const char *path, const std::string &text);

/** Strip leading/trailing whitespace (spaces, tabs, CR, LF). */
std::string str_trim(const std::string &s);

/** Case-insensitive ASCII compare. */
bool str_ieq(const std::string &a, const char *b);

/** Call fn(line) for each line (without the line ending). */
void for_each_line(const std::string &text, const std::function<void(const std::string &line)> &fn);

/**
 * INI-style "key=value" lines: ';' starts a comment (also mid-line), "[name]" sets the section,
 * keys and values are trimmed. fn(section, key, value) for each pair; section is "" before any
 * header.
 */
void ini_parse(const std::string &text,
               const std::function<void(const std::string &section, const std::string &key,
                                        const std::string &value)> &fn);
