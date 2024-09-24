cmake_minimum_required(VERSION 3.19.0)

project(COMPILER)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZOMPILER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(ZOMPILER_CMAKE_DIR ${ZOMPILER_ROOT}/cmake)

include(${ZOMPILER_CMAKE_DIR}/options.cmake)
include(${ZOMPILER_CMAKE_DIR}/common.cmake)

include_directories(${ZOMPILER_ROOT})

add_subdirectory(src)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
