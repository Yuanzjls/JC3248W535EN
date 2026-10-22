#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

/*
 * MP3 playback to the on-board NS4168 amplifier (I2S, speaker connector). A background task on
 * core 1 reads from the SD card, decodes (minimp3) and writes to I2S, so music keeps playing
 * while other apps are open. All functions are safe to call from any task.
 */

enum class AudioState { Stopped, Playing, Paused };

struct AudioStatus {
    AudioState state = AudioState::Stopped;
    size_t index = 0; /* current track in the playlist */
    size_t count = 0; /* playlist length */
    std::string path; /* card path of the current track ("" if none) */
    std::string title; /* ID3 title, else the file name */
    std::string artist; /* ID3 artist, may be empty */
    float pos_s = 0;
    float dur_s = 0; /* 0 if unknown */
    uint32_t sample_rate = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t cover_off = 0; /* embedded picture (ID3 APIC): JPEG / PNG bytes in the file */
    uint32_t cover_len = 0; /* 0 = none */
    bool error = false; /* last track could not be opened / decoded */
};

/** True for names the player can play (.mp3). */
bool audio_is_mp3(const char *name);

/** Play `playlist` (card paths) starting at `index`; replaces the current playlist. */
void audio_play(const std::vector<std::string> &playlist, size_t index);

/** Pause / resume (starts the current track again if stopped). */
void audio_toggle_pause(void);
void audio_next(void);
/** Previous track, or back to the start if more than a few seconds in. */
void audio_prev(void);
void audio_stop(void);
/** Jump to a fraction (0..1) of the current track. */
void audio_seek(float fraction);

/** Volume 0..100 (applies immediately; settings_set_volume() also saves it). */
void audio_set_volume(int percent);

/**
 * Shuffle: play the playlist in a random order, each track once before any repeats (Next, Prev and
 * the end of a track follow that order). Switching keeps the current track playing. Applies
 * immediately; settings_set_shuffle() also saves it.
 */
void audio_set_shuffle(bool on);

void audio_get_status(AudioStatus *out);
std::vector<std::string> audio_playlist(void);
