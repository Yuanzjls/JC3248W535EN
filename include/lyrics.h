#pragma once

#include <stdint.h>
#include <string>
#include <vector>

/*
 * Lyrics for the Music app, found in a background task:
 *   1. "<song>.lrc" next to the MP3 (your own files, or ones downloaded earlier);
 *   2. LRCLIB (https://lrclib.net, free, no account) by title / artist / duration - the result is
 *      saved as "<song>.lrc", so each song is downloaded once. Songs with no match get a marker in
 *      /.cache/lyrics/ so they aren't searched again (a retry request removes it).
 * Waits for Wi-Fi if it isn't up yet. Safe to call from the LVGL task.
 */

enum class LyricsKind { None, Synced, Plain };

struct LyricLine {
    int32_t ms;     /* start time (Synced); -1 for Plain */
    uint32_t off;   /* text offset in LyricsResult::text */
    uint32_t word0; /* first word in LyricsResult::words */
    uint32_t nwords; /* 0: no word timing (enhanced LRC "<mm:ss.xx>" markers) on this line */
};

/** A word's start in an enhanced-LRC line (the markers themselves are removed from the text). */
struct LyricWord {
    int32_t ms;
    uint32_t pos; /* byte offset of the word in the line's text */
};

struct LyricsResult {
    uint32_t id = 0;
    LyricsKind kind = LyricsKind::None;
    std::vector<LyricLine> lines; /* sorted by time (Synced); empty text = a pause */
    std::vector<LyricWord> words; /* word timing of all lines, see LyricLine::word0 */
    std::string text;             /* all line texts, each NUL-terminated (one buffer: few allocations) */
    std::string note;             /* None: what to show instead ("No lyrics found", ...) */
    bool final = true;            /* false: an interim note (e.g. waiting for Wi-Fi); more will follow */
    bool can_retry = false;       /* None: offer "Search again" */

    const char *line(size_t i) const { return text.data() + lines[i].off; }
};

/**
 * Find lyrics for a track (card path, title / artist as shown by the player - artist may be empty,
 * duration 0 if unknown). Replaces a request that hasn't finished. `retry` forgets an earlier
 * "not found". The result comes back from lyrics_take() with the same id.
 */
void lyrics_request(const std::string &path, const std::string &title, const std::string &artist, float dur_s,
                    uint32_t id, bool retry);

/** The newest result, if any (caller deletes it); nullptr if none is ready. */
LyricsResult *lyrics_take(void);
