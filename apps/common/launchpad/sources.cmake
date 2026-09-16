#
# The Launchpad protocol, as one list. Included by an app's main/CMakeLists.txt
# the same way apps/common/turn's is; see the note there about why neither
# directory has a CMakeLists.txt of its own.
#
# It is in common/ rather than inside the app that currently uses it because it
# is not app code: it is a port of the device driver from the STM32 tree, where
# it already lived in lib/surface/ for the same reason. The next thing that
# wants a control surface - and on this machine that is a synth - should find
# it here rather than write it again from the manuals.
#
set(_lp "${CMAKE_CURRENT_LIST_DIR}")

set(LP_SRCS "${_lp}/launchpad.c")
set(LP_INCLUDE_DIRS "${_lp}")
