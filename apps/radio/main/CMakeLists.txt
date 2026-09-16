idf_component_register(SRCS "radio_app.c"
                            "rad_util.c"
                            "rad_stations.c"
                            "rad_stream.c"
                            "rad_mp3.c"
                            "rad_audio.c"
                            "rad_eq.c"
                            "rad_ui.c"
                            "rad_eqpage.c"
                       INCLUDE_DIRS "."
                       REQUIRES ngl neos_api)

#
# -Werror=double-promotion, for the reason the clock app gives: apps link
# -nostdlib with no libgcc, and the loader's table carries a handful of the
# double helpers out of the dozens that exist, so one stray promotion is an
# app that will not load rather than one that runs slowly.
#
# It costs more here than in most apps, because this one is genuinely full of
# real numbers - biquad coefficients, a resampler's position, a tape deck's
# geometry. All of it is single precision on a chip that has a
# single-precision FPU, and all of it goes through the float half of <math.h>
# that ABI 1.21 exports. The flag is what keeps a bare 2.0 in a coefficient
# from reaching for sin() instead of sinf() and finding out at load time.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion)

#
# ... except over the decoder, which is vendored. minimp3 is public domain and
# fixed in its habits; holding it to our warning is how a vendored file ends up
# quietly edited, so its one translation unit is exempted instead. See
# rad_mp3.c.
#
set_source_files_properties(rad_mp3.c PROPERTIES
                            COMPILE_OPTIONS "-Wno-double-promotion")
