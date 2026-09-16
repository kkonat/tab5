idf_component_register(SRCS "wifiscan_app.c"
                            "ws_model.c"
                            "ws_plot.c"
                       INCLUDE_DIRS "."
                       REQUIRES ngl neos_api)

#
# -Werror=double-promotion, for the reason the clock app gives: apps link
# -nostdlib with no libgcc, and the loader's table carries a handful of the
# double helpers out of the dozens that exist, so one stray promotion is an
# app that will not load rather than one that runs slowly.
#
# This one does trade in real numbers - a Gaussian is a Gaussian - so the flag
# is doing work here rather than being a formality. Everything below stays in
# the float half of <math.h>, which is what the ABI exports and the P4's FPU
# is: one missing "f" on a literal turns a bell curve into a link error, which
# is the failure this wants.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion)
