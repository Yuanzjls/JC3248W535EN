# Main component: every source in src/, headers in include/.
# Libraries from platformio.ini lib_deps (LVGL, ArduinoJson, JPEGDEC, PNGdec) are the components in
# components/.
# ESP-IDF components (esp-nimble-cpp, esp_new_jpeg) come from idf_component.yml (which also makes
# them requirements of this component).
file(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)

# REQUIRES: the ESP-IDF components the sources include. Only these (and what they need) are built,
# see the top-level CMakeLists.txt.
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "../include"
                       REQUIRES
                           lvgl ArduinoJson jpegdec pngdec  # lib_deps (components/)
                           app_update                  # OTA slots and rollback (ota_update)
                           bootloader_support          # image header check (ota_update)
                           bt                          # BLE controller + NimBLE host (Air)
                           esp_driver_i2c              # touch (esp_bsp.c)
                           esp_driver_gpio
                           esp_driver_i2s              # speaker (audio_player)
                           esp_driver_ledc             # backlight
                           esp_driver_sdmmc
                           esp_driver_sdspi
                           esp_driver_spi              # display (QSPI)
                           esp_driver_usb_serial_jtag  # serial log
                           esp_event
                           esp_http_client             # weather, lyrics, radio
                           esp_http_server             # web page
                           esp_lcd
                           esp_netif
                           esp_psram
                           esp_timer
                           esp_wifi
                           espcoredump                 # core dump to flash on a panic (esp-coredump)
                           esp-tls
                           fatfs
                           lwip                        # SNTP
                           mbedtls                     # certificate bundle (HTTPS)
                           nvs_flash
                           sdmmc)

# The SD card doesn't answer ESP-IDF's SD Status Register read in SPI mode; let init continue
# without it (see __wrap_sdmmc_init_sd_ssr in sd_card.cpp).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_init_sd_ssr")

# MP3 decoding is the hot loop of the Music app: optimize it for speed, not size.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/minimp3/minimp3.c PROPERTIES COMPILE_OPTIONS "-O2")
