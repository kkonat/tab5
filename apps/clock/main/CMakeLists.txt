idf_component_register(SRCS "clock_app.c"
                            "clock_city.c"
                            "clock_draw.c"
                            "clock_dots.c"
                            "clock_text.c"
                            "clock_prefs.c"
                            "clock_rain.c"
                            "clock_wx.c"
                            "face_neos.c"
                            "face_led.c"
                            "face_lcd.c"
                            "face_vfd.c"
                            "face_epaper.c"
                            # Generated. See tools/genkanji and tools/genicons.
                            "clock_kanji.c"
                            "clock_icon_data.c"
                       INCLUDE_DIRS "."
                       REQUIRES ngl neos_api)

#
# -Werror=double-promotion, because a double is not slow here, it is fatal.
#
# Apps link -nostdlib with no libgcc, and the loader's symbol table carries a
# handful of double helpers out of the dozens that exist. One stray `1.0`
# instead of `1.0f` - or one float that gets promoted on its way into printf -
# leaves an undefined symbol, which first shows up as the app refusing to load
# on the tablet rather than as anything a build says. This turns it into a
# compile error instead.
#
# Nothing here wants a float in the first place: the accelerometer arrives in
# milli-g and the temperature in tenths of a degree, both integers by the time
# they cross the ABI, and every scale factor in the drawing is a ratio.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion)
