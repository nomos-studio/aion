# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(aion
    VERSION 0.1.0
    DESCRIPTION "aion — nomos-rt session substrate: Link peer, MIDI/OSC, txlog"
    HOMEPAGE_URL "https://github.com/nomos-studio/aion"
    LANGUAGES C CXX)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip network fetch if already populated")

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

option(AION_BUILD_TESTS "Build aion unit tests" OFF)

# ---------------------------------------------------------------------------
# nomos-rt — shared C++ runtime substrate.
#
# To use a local checkout (avoids network and speeds up iteration):
#   cmake -DNOMOS_RT_DIR=/path/to/nomos-rt ...
# ---------------------------------------------------------------------------

if(DEFINED NOMOS_RT_DIR)
    FetchContent_Declare(nomos-rt SOURCE_DIR "${NOMOS_RT_DIR}")
else()
    FetchContent_Declare(nomos-rt
        GIT_REPOSITORY https://github.com/nomos-studio/nomos-rt.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE)
endif()
set(NOMOS_RT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nomos-rt)

# ---------------------------------------------------------------------------
# aion executable
# ---------------------------------------------------------------------------

add_executable(aion
    src/main.cpp
    src/version.cpp
)

# nomos::rt          — public headers + all runtime libs (Link, RtMidi, etc.)
# nomos::rt-exe-headers — exposes nomos-rt src/ headers: link_peer, midi_io,
#                         osc_server, audio_device (all GPL-2.0-or-later)
target_link_libraries(aion PRIVATE
    nomos::rt
    nomos::rt-exe-headers
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
