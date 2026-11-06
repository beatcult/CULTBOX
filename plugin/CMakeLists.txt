# SPDX-License-Identifier: GPL-3.0-only
# CULTBOX PLVKS as a CLAP plugin (the firmware's Polivoks voice and BBD echo, from ../firmware/src).
#   cmake -S plugin -B build/plugin [-DCLAP_INCLUDE_DIR=/path/to/clap/include] && cmake --build build/plugin
# -> build/plugin/cultbox-plvks.clap. Without CLAP_INCLUDE_DIR the CLAP headers (MIT) are fetched from GitHub (tag 1.2.2).
# Needs a C11 compiler: GCC or Clang (Linux, macOS, MinGW), not MSVC.
cmake_minimum_required(VERSION 3.16)
project(cultbox_plvks C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

set(CLAP_INCLUDE_DIR "" CACHE PATH "directory holding clap/clap.h")
if(NOT CLAP_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap.git GIT_TAG 1.2.2 GIT_SHALLOW TRUE)
    FetchContent_GetProperties(clap)
    if(NOT clap_POPULATED)
        FetchContent_Populate(clap)
    endif()
    set(CLAP_INCLUDE_DIR "${clap_SOURCE_DIR}/include")
endif()

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)
add_library(cultbox-plvks MODULE clap/cultbox_plvks.c ${FW}/polivoks.c ${FW}/polivoks_fx.c)
target_include_directories(cultbox-plvks PRIVATE ${FW} ${CLAP_INCLUDE_DIR})
target_compile_definitions(cultbox-plvks PRIVATE POLIVOKS_FX_NO_LEGACY)
target_compile_options(cultbox-plvks PRIVATE -Wall -Wextra -Wno-unused-function)
target_link_libraries(cultbox-plvks PRIVATE m)
set_target_properties(cultbox-plvks PROPERTIES PREFIX "" SUFFIX ".clap")
