# Builds umd_tile_events against a tt-umd checkout, which is built here alongside it.
#   cmake -B build -G Ninja -DTT_UMD_DIR=/path/to/tt-umd && cmake --build build
cmake_minimum_required(VERSION 3.24)
project(umd_tile_events LANGUAGES C CXX)

set(TT_UMD_DIR "" CACHE PATH "tt-umd checkout with DeviceEventReader (tt-umd stack #3793)")
if(NOT EXISTS "${TT_UMD_DIR}/tt-umd/api/tt-umd/pcie/device_event_reader.hpp")
    message(FATAL_ERROR "Set TT_UMD_DIR to a tt-umd checkout that has DeviceEventReader")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Just the library: UMD's tools, tests and clang-tidy are not needed here.
set(TT_UMD_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(TT_UMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TT_UMD_ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
add_subdirectory(${TT_UMD_DIR} ${CMAKE_BINARY_DIR}/tt-umd EXCLUDE_FROM_ALL)

add_executable(umd_tile_events umd_tile_events.cpp)
target_include_directories(umd_tile_events PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(umd_tile_events PRIVATE umd::tt-umd)
