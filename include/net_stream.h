#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

/*
 * Internet radio source for the audio player: a background task pulls an HTTP(S) MP3 stream into
 * a PSRAM ring buffer - following redirects, stripping ICY metadata (the "now playing" title), and
 * reconnecting after drops - and the player reads from the buffer. Playback starts once a few
 * seconds are buffered, and pauses to rebuffer if the buffer runs dry.
 */

enum class StreamState { Idle, Connecting, Buffering, Playing, Retrying, Failed };

struct StreamInfo {
    StreamState state = StreamState::Idle;
    std::string now_playing; /* ICY StreamTitle, may be empty */
    std::string error;       /* Failed / Retrying: why */
    int buffer_pct = 0;      /* fill of the ring buffer */
};

/** Start streaming `url`, replacing any current stream. */
void stream_open(const std::string &url);

/** Stop the current stream. */
void stream_close(void);

/** Up to len bytes of MP3 data; waits at most timeout_ms. 0 while connecting or buffering. */
size_t stream_read(uint8_t *dst, size_t len, uint32_t timeout_ms);

/** The stream gave up (station unreachable, unsupported format). */
bool stream_failed(void);

void stream_get_info(StreamInfo *out);
