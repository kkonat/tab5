idf_component_register(SRCS "neos_main.c"
                            "neos_boot.c"
                            "neos_app.c"
                            "neos_apps.c"
                            "neos_cfg.c"
                            "neos_crash.c"
                            "neos_msg.c"
                            "neos_syscalls.c"
                            "neos_orient.c"
                            "neos_sys.c"
                            "neos_bar.c"
                            "neos_status.c"
                            "neos_touch.c"
                            "neos_audio.c"
                            "neos_ui.c"
                            "neos_kbd.c"
                            "neos_time.c"
                            "neos_net.c"
                            "neos_sock.c"
                            "neos_weather.c"
                            "neos_panel_clock.c"
                            "neos_panel_wifi.c"
                            "neos_panel_battery.c"
                            "neos_upload.c"
                            "neos_screencap.c"
                       INCLUDE_DIRS "."
                       REQUIRES json nvs_flash esp_lcd esp_lcd_touch ngl neos_api bmi270 esp_timer sdmmc
                                esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_tsens
                                esp_io_expander espressif__esp_io_expander_pi4ioe5v6408 esp_codec_dev esp_wifi esp_netif esp_event lwip
                                # The weather service: one plain GET and one over TLS
                                # against the IDF certificate bundle.
                                esp_http_client esp-tls mbedtls
                                esp_app_format fatfs
                                # The radio is a second chip on the far side of SDIO, and taking
                                # its power away means tearing that transport down first - which
                                # is esp_hosted's own API, not esp_wifi's.
                                espressif__esp_hosted)

# Where neos_build.h lands. Generated, so it stays out of the source tree.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}/generated")

# This side defines the ABI guard objects; only apps emit a reference to one.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NEOS_ABI_HOST=1)
