# SPDX-License-Identifier: GPL-3.0-only
# CULTBOX engines as plugins (the firmware's own voices and BBD echo, from ../firmware/src):
#   cultbox-plvks       the Polivoks voice
#   cultbox-dubmarine   the dub siren
#
#   cmake -S plugin -B build/plugin [-DCLAP_INCLUDE_DIR=/path/to/clap/include] && cmake --build build/plugin
#       -> build/plugin/cultbox-plvks.clap and cultbox-dubmarine.clap
#   cmake -S plugin -B build/plugin -DCULTBOX_WRAPPER=ON && cmake --build build/plugin --target cultbox-plvks_all cultbox-dubmarine_all
#       -> also VST3 (and AUv2 on macOS), through clap-wrapper (MIT, fetched at configure time with the VST3 SDK, MIT too, and on
#          macOS Apple's AudioUnitSDK); the files are collected in build/plugin/plugins/
# Without CLAP_INCLUDE_DIR the CLAP headers (MIT) are fetched from GitHub (tag 1.2.10).
# Needs a C11 compiler (GCC or Clang; on Windows clang-cl, not MSVC's own cl), and with CULTBOX_WRAPPER a C++17 one.
cmake_minimum_required(VERSION 3.21)
project(cultbox_plugins C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CULTBOX_WRAPPER "Also build VST3 (and AUv2 on macOS) with clap-wrapper" OFF)
set(CULTBOX_CLAP_WRAPPER_TAG v0.17.0 CACHE STRING "clap-wrapper release to fetch")
set(CLAP_INCLUDE_DIR "" CACHE PATH "directory holding clap/clap.h")
set(CLAP_VERSION_TAG 1.2.10)
include(FetchContent)
if(NOT CLAP_INCLUDE_DIR)
    FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap.git GIT_TAG ${CLAP_VERSION_TAG} GIT_SHALLOW TRUE)
    FetchContent_GetProperties(clap)
    if(NOT clap_POPULATED)
        FetchContent_Populate(clap)
    endif()
    set(CLAP_INCLUDE_DIR "${clap_SOURCE_DIR}/include")
endif()

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)

if(CULTBOX_WRAPPER)
    enable_language(CXX)
    if(APPLE)
        enable_language(OBJC OBJCXX)                 # the Audio Unit and VST3 wrappers have Objective-C++ parts
    endif()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static runtime everywhere: one kind (clap-wrapper's pieces must agree), and no VC++ redistributable to install
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
    endif()
    get_filename_component(CLAP_SDK_ROOT "${CLAP_INCLUDE_DIR}/.." ABSOLUTE)      # clap-wrapper wants the SDK's root
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)               # the VST3 SDK (and on macOS the AudioUnitSDK)
    set(CLAP_WRAPPER_COPY_AFTER_BUILD OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(clap_wrapper GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git GIT_TAG ${CULTBOX_CLAP_WRAPPER_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap_wrapper)
endif()

# the sources of one plugin: the shared wrapper, an engine file, the echo, and the firmware sources that engine needs
function(cultbox_sources out engine_file)
    set(${out} clap/cultbox_clap.c clap/${engine_file} ${FW}/polivoks_fx.c ${ARGN} PARENT_SCOPE)
endfunction()
function(cultbox_settings target)
    target_include_directories(${target} PRIVATE clap ${FW} ${CLAP_INCLUDE_DIR})
    # CULTBOX_REENTRANT: scratch on the stack, so instances may render on different threads at once
    target_compile_definitions(${target} PRIVATE POLIVOKS_FX_NO_LEGACY CULTBOX_REENTRANT)
    target_compile_options(${target} PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wno-unused-function>)
    if(NOT WIN32)
        target_link_libraries(${target} PRIVATE m)
    endif()
endfunction()

function(cultbox_plugin name output engine_file auv2_subtype)
    cultbox_sources(srcs ${engine_file} ${ARGN})
    if(NOT CULTBOX_WRAPPER)
        add_library(${name} MODULE ${srcs})
        cultbox_settings(${name})
        set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME ${output})
        return()
    endif()
    # clap-first: the plugin as a static library plus a small entry file; clap-wrapper makes the CLAP, VST3 and AUv2 from them
    add_library(${name}-impl STATIC ${srcs})
    cultbox_settings(${name}-impl)
    target_compile_definitions(${name}-impl PRIVATE CULTBOX_CLAP_FIRST)
    target_link_libraries(${name}-impl PUBLIC clap clap-wrapper-extensions)   # (so the entry file, built for each format, finds the CLAP headers)
    set(formats CLAP VST3)
    if(APPLE)
        list(APPEND formats AUV2)
    endif()
    make_clapfirst_plugins(
        TARGET_NAME ${name}
        IMPL_TARGET ${name}-impl
        OUTPUT_NAME ${output}
        ENTRY_SOURCE clap/entry.cpp
        BUNDLE_IDENTIFIER "com.beatcult.${output}"
        BUNDLE_VERSION 0.1.0
        COPY_AFTER_BUILD FALSE
        WINDOWS_FOLDER_VST3 FALSE
        PLUGIN_FORMATS ${formats}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
        AUV2_MANUFACTURER_NAME "beatcult"
        AUV2_MANUFACTURER_CODE "BtCl"
        AUV2_SUBTYPE_CODE ${auv2_subtype}
        AUV2_INSTRUMENT_TYPE "aumu")
endfunction()

cultbox_plugin(cultbox-plvks cultbox-plvks plvks.c Plvk ${FW}/polivoks.c)
cultbox_plugin(cultbox-dubmarine cultbox-dubmarine dubmarine.c DbMr ${FW}/dubsiren.c)
