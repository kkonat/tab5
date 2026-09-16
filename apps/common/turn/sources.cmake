#
# The turned canvas, as one list.
#
# Included by an app's main/CMakeLists.txt, the way lab/defender includes its
# own game/sources.cmake. Paths are relative to the component that includes
# this, which is always main/ - so an app two levels under the repo root reaches
# it with ../../common/turn, and an app anywhere else does not, which is the
# same depth rule apps already live by.
#
# There is deliberately no CMakeLists.txt here. scripts/build-all.ps1 builds
# every directory under apps/ that has one, and this is a piece of an app
# rather than an app.
#
set(_turn "${CMAKE_CURRENT_LIST_DIR}")

set(TURN_SRCS "${_turn}/turn.c")
set(TURN_INCLUDE_DIRS "${_turn}")
