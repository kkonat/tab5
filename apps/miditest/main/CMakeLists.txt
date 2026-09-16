#
# The Launchpad protocol is shared rather than copied in here. It is a port of
# the driver from the STM32 tree and it is device knowledge, not app logic; see
# apps/common/launchpad.
#
include("${CMAKE_CURRENT_LIST_DIR}/../../common/launchpad/sources.cmake")

idf_component_register(SRCS "miditest_app.c"
                            ${LP_SRCS}
                       INCLUDE_DIRS "." ${LP_INCLUDE_DIRS}
                       REQUIRES ngl neos_api)

#
# -Werror=double-promotion, for the reason the clock app gives: apps link
# -nostdlib with no libgcc, and the loader's table carries a handful of the
# double helpers out of the dozens that exist, so one stray promotion is an
# app that will not load rather than one that runs slowly.
#
# There is not a real number anywhere in this app - a velocity is a velocity
# and an LED index is two decimal digits - so the flag is here to keep it that
# way rather than to catch anything.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion)
