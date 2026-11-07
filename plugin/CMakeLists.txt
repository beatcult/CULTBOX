# SPDX-License-Identifier: GPL-3.0-only
# CULTBOX engines as CLAP plugins (the firmware's own voices and BBD echo, from ../firmware/src):
#   cultbox-plvks.clap       the Polivoks voice
#   cultbox-dubmarine.clap   the dub siren
#   cmake -S plugin -B build/plugin [-DCLAP_INCLUDE_DIR=/path/to/clap/include] && cmake --build build/plugin
# Without CLAP_INCLUDE_DIR the CLAP headers (MIT) are fetched from GitHub (tag 1.2.2).
# Needs a C11 compiler: GCC or Clang (Linux, macOS, MinGW), not MSVC.
cmake_minimum_required(VERSION 3.16)
project(cultbox_plugins C)
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

# one plugin: the shared wrapper, an engine file, and the firmware sources that engine needs
function(cultbox_plugin target engine_file)
    add_library(${target} MODULE clap/cultbox_clap.c clap/${engine_file} ${FW}/polivoks_fx.c ${ARGN})
    target_include_directories(${target} PRIVATE clap ${FW} ${CLAP_INCLUDE_DIR})
    # CULTBOX_REENTRANT: scratch on the stack, so instances may render on different threads at once
    target_compile_definitions(${target} PRIVATE POLIVOKS_FX_NO_LEGACY CULTBOX_REENTRANT)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-function)
    target_link_libraries(${target} PRIVATE m)
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap")
endfunction()

cultbox_plugin(cultbox-plvks plvks.c ${FW}/polivoks.c)
cultbox_plugin(cultbox-dubmarine dubmarine.c ${FW}/dubsiren.c)
