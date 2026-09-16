#
# The turned canvas and the widgets are shared with apps/synth1 rather than
# copied into both: the first is subtle coordinate code with a test suite
# behind it, and the second is what stops two synthesisers on one machine
# being drawn in two different hands. See apps/common.
#
include("${CMAKE_CURRENT_LIST_DIR}/../../common/turn/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../common/widget/sources.cmake")

idf_component_register(SRCS "mg_app.c"
                            "mg_ui.c"
                            "mg_panel.c"
                            "mg_dsp.c"
                            "mg_audio.c"
                            ${TURN_SRCS}
                            ${WG_SRCS}
                       INCLUDE_DIRS "." ${TURN_INCLUDE_DIRS} ${WG_INCLUDE_DIRS}
                                    "../../common/dsp"
                       REQUIRES ngl neos_api)

#
# -O2, because the two loops this app exists to run are both hot: the voice
# writes 48,000 samples a second through three oscillators and a four-pole
# ladder, and the turned canvas writes every pixel of a repainted cell with the
# CPU. The project builds -Og, which is the right default for UI code and the
# wrong one for either of those; a later -O overrides an earlier one, so this
# is the whole change.
#
# -Werror=double-promotion, because a double here is not slow, it is fatal:
# apps link -nostdlib with no compiler runtime, so one stray `1.0` instead of
# `1.0f` leaves an undefined symbol that first shows up as the app refusing to
# load on the tablet. See lab/defender/docs/porting.md.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Werror=double-promotion)
