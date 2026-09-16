cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../neos/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(miditest)

# Provided by the elf_loader component: emits miditest.app.elf instead of a firmware
include(elf_loader)
project_elf(miditest)
