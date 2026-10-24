# JC3248W535EN - ESP32-S3 touch panel firmware

Firmware for the Guition JC3248W535EN (a.k.a. "cheap black board"): ESP32-S3, 16 MB flash,
8 MB octal PSRAM, 3.5" 480x320 QSPI display (AXS15231B) with capacitive touch, microSD slot.

A small launcher with apps: Wi-Fi setup, Clock, Weather (Open-Meteo), Files (browser, image
viewer, MJPEG video player), Music (MP3 player with lyrics), Radio (internet radio), Breakout, Air
(Airthings Wave radon sensor over BLE) and Settings.
UI with LVGL 8.3.

## Build

Plain ESP-IDF 6.1 (CMake / FreeRTOS), built with PlatformIO 6.2 or newer (`framework = espidf`,
pioarduino's platform - see `platformio.ini`). On Windows, enable long paths once (ESP-IDF 6.1
has paths over 260 characters): `LongPathsEnabled` = 1 in
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`, as administrator. PlatformIO
supplies ESP-IDF, the tools and the libraries; `scripts/idf_build.py` then builds with ESP-IDF's
own CMake + Ninja build (as `idf.py build` does) into `.pio/build/JC3248W535EN/idf/`.

```
pio run            # build
pio run -t upload  # flash over USB
pio device monitor # logs (115200, USB Serial/JTAG)
```

On Windows run PlatformIO from PowerShell or cmd: the ESP-IDF build refuses Git Bash / MSYS.

Over-the-air update: once the board runs a firmware with OTA support (flash it over USB once),
open its web page (`http://<board-IP>/`, shown in Settings) and use **Firmware > Update
firmware...** with `.pio/build/JC3248W535EN/firmware.bin`. The image goes to the spare app slot
(`app0` / `app1`), is verified, and the board restarts into it. A new firmware confirms itself
after a minute of normal running; if it crashes or resets before then, the bootloader goes back
to the previous one. Only firmware built for this project is accepted. The web page has no
login - anyone on your network could update the board, as they can already use the file manager.
The firmware version shown (web page, Settings > About) is `git describe` at build time.

- Configuration: `sdkconfig.defaults` - only the options that differ from ESP-IDF's defaults. The
  build expands it into the full `sdkconfig.JC3248W535EN` (generated, not versioned); delete that
  file after editing `sdkconfig.defaults` so the change is picked up.
- Components: only those `src/CMakeLists.txt` requires are built (`COMPONENTS` in the top-level
  `CMakeLists.txt`); add a component there when the code starts using one.
- ESP-IDF components (`esp-nimble-cpp`, `esp_new_jpeg`): `src/idf_component.yml`, fetched into
  `managed_components/` on the first build.
- Libraries (LVGL, ArduinoJson, JPEGDEC, PNGdec): `lib_deps` in `platformio.ini`, downloaded by
  PlatformIO into `.pio/libdeps/` and compiled as the components in `components/` (their compile
  flags are there too).
- LVGL config: `include/lv_conf.h` (found through `LV_CONF_INCLUDE_SIMPLE`, nothing to copy).

## microSD card

Everything persistent lives on the card (FAT), nothing in internal flash:

| File | |
|---|---|
| `fonts/UI_FONT_16.bin` | UI font with CJK (copy from `documentation/fonts/`) |
| `wifi_known.txt` | saved Wi-Fi networks (written by the Wi-Fi app) |
| `settings.ini`, `timezone.ini` | brightness, units, clock format, speaker volume, shuffle, timezone |
| `music/` | MP3 files for the Music app (subfolders too) |
| `radio.txt` | saved radio stations, one per line: `<stream URL> <name>` (written by the Radio app) |
| `radio_last.txt` | last radio station played |
| `weather.ini` | optional: `lat`, `lon`, `name`, `units` (otherwise IP geolocation) |
| `airthings.ini` | chosen radon sensor (written by the Air app) |
| `breakout.ini` | high score |
| `.cache/mjpeg/` | video frame indexes (rebuilt if deleted) |

Music: MP3 (MPEG-1/2/2.5, CBR or VBR), decoded with minimp3 (vendored in `src/minimp3/`); ID3
title, artist and embedded cover art (JPEG / PNG) are shown. Playback keeps
going in the background; an `.mp3` opened in Files plays that folder. The speaker (2-pin
"Speak" connector) is driven by the on-board NS4168 mono I2S amplifier (BCLK IO42, LRCLK IO2,
DIN IO41), so stereo is mixed down to mono.

Lyrics: a `<song>.lrc` next to the MP3 is used as is (UTF-8; line-synced, word-synced
"enhanced LRC" or plain text). Otherwise they are looked up on [LRCLIB](https://lrclib.net) by
title, artist and duration (needs Wi-Fi) and saved as `<song>.lrc`; songs with no match are
remembered in `.cache/lyrics/`. Title and artist come from the ID3 tag, else from an
"Artist - Title" file name - use the names LRCLIB knows (e.g. the original-language title).

Radio: MP3 internet radio over HTTP / HTTPS (AAC and HLS streams aren't supported). Stations come
from `radio.txt` (Saved) or the [Radio Browser](https://www.radio-browser.info) directory: Local
(the most played stations in your country, by IP address) and Top (worldwide). Playback starts
after ~2 s of buffering, shows the station's "now playing" title (ICY metadata) and reconnects
after drops.

At high volume on a PC's USB port, the amplifier can pull the 5 V supply down far enough that
the board drops off USB and restarts (reset reason "USB reset", no panic). Use a charger or the
battery for loud playback.

Videos: raw MJPEG (`.mjpeg` / `.mjpg`), ideally 480x320; `_15fps` in the file name sets the rate
(default 24).

## Hardware

Datasheets and drawings from the manufacturer are in `documentation/`.

<img src="images/front.jpg" alt="Front view" width="45%"> <img src="images/back.jpg" alt="Back view" width="45%">

<img src="images/JC3248W535-1.png" alt="Datasheet page 1" width="45%"> <img src="images/JC3248W535-2.png" alt="Datasheet page 2" width="45%">
