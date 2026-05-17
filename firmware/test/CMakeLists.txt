cmake_minimum_required(VERSION 3.16)

# Reuse application components from ../components
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../components")

set(TEST_COMPONENTS "cybeer_switch;cybeer_fsm" CACHE STRING "Components whose test/ subdir is built")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cybeer_switch_unity)
