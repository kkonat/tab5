cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(neos)

# How many neighbours the stack remembers.
#
# lwIP's default is 10, which is right for a device that talks to a gateway
# and a server. It is wrong the moment something sweeps the subnet: sending to
# 254 addresses means resolving 254 addresses, and neos_sock.h hands an app
# that table as the answer to "who is out there" - a MAC being the one id that
# survives a lease change. At 10 the answer would be whichever eight replies
# arrived last.
#
# No Kconfig for it, so it is set here. PUBLIC and not PRIVATE on purpose: the
# array lives in lwIP's own etharp.c but neos_sock.c walks it by index, and
# the two disagreeing about how long it is would be a read past the end that
# only shows up on a busy network.
idf_component_get_property(neos_lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${neos_lwip_lib} PUBLIC ARP_TABLE_SIZE=64)

# The build number in the system bar.
#
# A target rather than a configure-time step: CMake reconfigures rarely and a
# number that only moved then would name the wrong image. ALL plus no OUTPUT
# makes it always out of date, which is exactly what "once per build" means.
# main depends on it so the header exists before anything includes it.
add_custom_target(neos_build_number ALL
    COMMAND ${CMAKE_COMMAND}
            -DCOUNTER=${CMAKE_CURRENT_LIST_DIR}/build_number.txt
            -DHEADER=${CMAKE_BINARY_DIR}/generated/neos_build.h
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/bump_build.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/neos_build.h
    COMMENT "NeOS build number")
add_dependencies(__idf_main neos_build_number)
