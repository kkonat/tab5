#
# The turned canvas is shared with apps/moog rather than copied into both.
# It is subtle coordinate code with a test suite behind it, and two copies of
# that in one repository is two copies that drift. See apps/common/turn.
#
include("${CMAKE_CURRENT_LIST_DIR}/../../common/turn/sources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../common/widget/sources.cmake")

idf_component_register(SRCS "syn_app.c"
                            "syn_ui.c"
                            "syn_dsp.c"
                            "syn_audio.c"
                            ${TURN_SRCS}
                            ${WG_SRCS}
                       INCLUDE_DIRS "." ${TURN_INCLUDE_DIRS} ${WG_INCLUDE_DIRS}
                                    "../../common/dsp"
                       REQUIRES ngl neos_api)

#
# -O2, because the two loops this app exists to measure are both hot: the voice
# writes 48,000 samples a second and the turned canvas writes every pixel of a
# repainted widget with the CPU. The project builds -Og, which is the right
# default for UI code and the wrong one for either of those; a later -O
# overrides an earlier one, so this is the whole change.
#
# -Werror=double-promotion, because a double here is not slow, it is fatal:
# apps link -nostdlib with no compiler runtime, so one stray `1.0` instead of
# `1.0f` leaves an undefined symbol that first shows up as the app refusing to
# load on the tablet. The DSP is single precision throughout and this is what
# keeps it that way. See lab/defender/docs/porting.md.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Werror=double-promotion)
