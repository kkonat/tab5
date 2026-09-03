cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(neos)

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
