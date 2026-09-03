idf_component_register(SRCS "neos_main.c"
                            "neos_boot.c"
                            "neos_app.c"
                            "neos_apps.c"
                            "neos_cfg.c"
                            "neos_crash.c"
                            "neos_msg.c"
                            "neos_syscalls.c"
                            "neos_orient.c"
                            "neos_bar.c"
                            "neos_status.c"
                            "neos_touch.c"
                            "neos_upload.c"
                       INCLUDE_DIRS "."
                       REQUIRES json nvs_flash esp_lcd esp_lcd_touch ngl neos_api bmi270 esp_timer sdmmc esp_driver_usb_serial_jtag)

# This side defines the ABI guard objects; only apps emit a reference to one.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NEOS_ABI_HOST=1)
