#
# The shared widgets, as one list. Included by an app's main/CMakeLists.txt
# the same way apps/common/turn's is; see the note there about why neither
# directory has a CMakeLists.txt of its own.
#
set(_wg "${CMAKE_CURRENT_LIST_DIR}")

set(WG_SRCS "${_wg}/wg.c")
set(WG_INCLUDE_DIRS "${_wg}")
