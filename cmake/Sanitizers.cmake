# SPDX-FileCopyrightText: 2026 nomos-studio contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(NOMOS_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers to enable: address, thread, undefined")

# Guard: if a parent project already defined the target (FetchContent), reuse it.
if(TARGET sanitizers::sanitizers)
    return()
endif()

add_library(_nomos_sanitizer_flags INTERFACE)
add_library(sanitizers::sanitizers ALIAS _nomos_sanitizer_flags)

if(NOT NOMOS_SANITIZE)
    # Define no-op function so call sites compile cleanly without sanitizers.
    function(nomos_sanitize_target target)
    endfunction()
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    message(WARNING
        "NOMOS_SANITIZE=${NOMOS_SANITIZE}: sanitizers are only supported with "
        "GCC or Clang. The sanitizers::sanitizers target is a no-op.")
    function(nomos_sanitize_target target)
    endfunction()
    return()
endif()

# Normalise to a CMake list (accept both comma and semicolon separators).
string(REPLACE "," ";" _san_list "${NOMOS_SANITIZE}")

if("address" IN_LIST _san_list AND "thread" IN_LIST _san_list)
    message(FATAL_ERROR
        "NOMOS_SANITIZE: 'address' and 'thread' are mutually exclusive. "
        "Choose one; both may be combined with 'undefined'.")
endif()

set(_san_compile "")
set(_san_link    "")

if("address" IN_LIST _san_list)
    list(APPEND _san_compile -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND _san_link    -fsanitize=address)
    message(STATUS "Sanitizers: AddressSanitizer enabled")
endif()

if("thread" IN_LIST _san_list)
    list(APPEND _san_compile -fsanitize=thread -fno-omit-frame-pointer)
    list(APPEND _san_link    -fsanitize=thread)
    message(STATUS "Sanitizers: ThreadSanitizer enabled")
    if(APPLE)
        message(STATUS
            "Sanitizers [TSan/macOS]: set MallocMaxMagazines=0 in the test env "
            "— see cmake/Sanitizers.cmake for details.")
    endif()
endif()

if("undefined" IN_LIST _san_list)
    list(APPEND _san_compile
        -fsanitize=undefined
        -fno-sanitize-recover=undefined)
    list(APPEND _san_link -fsanitize=undefined)
    message(STATUS "Sanitizers: UndefinedBehaviorSanitizer enabled (fatal mode)")
endif()

foreach(_s IN LISTS _san_list)
    if(NOT _s MATCHES "^(address|thread|undefined)$")
        message(WARNING "NOMOS_SANITIZE: unknown sanitizer '${_s}' (ignored)")
    endif()
endforeach()

if(NOT _san_compile)
    function(nomos_sanitize_target target)
    endfunction()
    return()
endif()

target_compile_options(_nomos_sanitizer_flags INTERFACE ${_san_compile})
target_link_options(   _nomos_sanitizer_flags INTERFACE ${_san_link})

# nomos_sanitize_target(<target>)
# Apply sanitizer compile flags to an exported static library target without
# creating a link dependency on sanitizers::sanitizers.  This avoids the CMake
# export-set error that occurs when an exported target has a PRIVATE dependency
# on a target not included in the install(EXPORT ...) set.
function(nomos_sanitize_target target)
    target_compile_options(${target} PRIVATE ${_san_compile})
endfunction()
