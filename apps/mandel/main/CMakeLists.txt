idf_component_register(SRCS "mandel_app.c" "fractal.c" "palette.c" "save.c"
                       INCLUDE_DIRS "."
                       REQUIRES ngl neos_api)

#
# -O2, because this app is one loop.
#
# The project builds -Og, which is the right default for UI code and the wrong
# one for an iteration kernel that runs a few hundred million times per frame:
# -O2 is worth roughly a factor of two on the escape loop, which comes straight
# off the wait after every zoom. A later -O overrides an earlier one, so this
# is the whole change.
#
# -Werror=double-promotion, because a double is not slow here, it is fatal.
# Apps link -nostdlib with no libgcc and the loader's table carries five double
# helpers out of the dozens that exist, so one stray `1.0` instead of `1.0f`
# leaves an undefined symbol that first shows up as the app refusing to load on
# the tablet. This turns it into a compile error instead.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Werror=double-promotion)
