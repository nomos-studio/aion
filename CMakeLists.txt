# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(aion
    VERSION 0.1.0
    DESCRIPTION "aion — cljseq-rt session substrate: Link peer, MIDI/OSC, txlog"
    HOMEPAGE_URL "https://github.com/cljseq/aion"
    LANGUAGES C CXX)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip network fetch if already populated")

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

option(AION_BUILD_TESTS "Build aion unit tests" OFF)

# ---------------------------------------------------------------------------
# kairos — fetched for cljseq-rt only; CLAP layer is not needed.
#
# To use a local kairos checkout (avoids network and speeds up iteration):
#   cmake -DKAIROS_DIR=/path/to/kairos ...
# ---------------------------------------------------------------------------

set(KAIROS_BUILD_CORE       OFF CACHE BOOL "" FORCE)
set(KAIROS_BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(KAIROS_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(KAIROS_WASM_BRIDGE      OFF CACHE BOOL "" FORCE)

if(DEFINED KAIROS_DIR)
    FetchContent_Declare(kairos SOURCE_DIR "${KAIROS_DIR}")
else()
    FetchContent_Declare(kairos
        GIT_REPOSITORY https://github.com/cljseq/kairos.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE)
endif()
FetchContent_MakeAvailable(kairos)

# ---------------------------------------------------------------------------
# aion executable
# ---------------------------------------------------------------------------

add_executable(aion
    src/main.cpp
    src/version.cpp
)

# cljseq::rt     — public kairos headers + all runtime libs (Link, RtMidi, etc.)
# cljseq::rt-exe-headers — exposes kairos src/ headers: link_peer, midi_io,
#                          osc_server, audio_device (all GPL-2.0-or-later)
target_link_libraries(aion PRIVATE
    cljseq::rt
    cljseq::rt-exe-headers
)

set_target_properties(aion PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_compile_options(aion PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
