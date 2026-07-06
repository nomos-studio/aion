# SPDX-License-Identifier: GPL-2.0-or-later
# cmake/Sanitizers.cmake — AddressSanitizer, ThreadSanitizer, UndefinedBehaviorSanitizer
#
# Usage — executables and test runners
# -------------------------------------
#   target_link_libraries(my-test PRIVATE sanitizers::sanitizers)
#
# Applies compile-time instrumentation AND links the sanitizer runtime.  Use
# this for all executables and test binaries.
#
# Usage — installed/exported static libraries
# --------------------------------------------
#   nomos_sanitize_target(my-lib)
#
# Applies only compile-time instrumentation.  Does NOT create a dependency on
# sanitizers::sanitizers so the target can be exported without errors.
# The final executable that links the library is responsible for linking the
# sanitizer runtime via sanitizers::sanitizers.
#
# Configure
# ---------
#   cmake -DNOMOS_SANITIZE=address,undefined ..   # ASan + UBSan  (most common)
#   cmake -DNOMOS_SANITIZE=thread,undefined ..    # TSan + UBSan
#   cmake -DNOMOS_SANITIZE=undefined ..           # UBSan only
#   cmake -DNOMOS_SANITIZE=address ..             # ASan only
#
# Notes
# -----
# - 'address' and 'thread' are mutually exclusive; CMake will error if both
#   are requested.
# - UBSan uses -fno-sanitize-recover=undefined so violations abort the process
#   rather than printing a warning and continuing.
# - -fno-omit-frame-pointer is added when ASan or TSan is active so that stack
#   traces are readable without debug info.
# - MSVC and non-GCC/Clang toolchains: a no-op sanitizers::sanitizers target is
#   created so consumers compile cleanly; a warning is emitted.
# - FetchContent sub-projects: if sanitizers::sanitizers already exists when
#   this file is included (because a parent project created it), the file
#   returns immediately — the parent's settings propagate through the build.
# - TSan on macOS requires MallocMaxMagazines=0 in the test environment;
#   CTest does not set this automatically.  Add it per-test with
#   set_tests_properties(<name> PROPERTIES ENVIRONMENT
#       "MallocMaxMagazines=0;TSAN_OPTIONS=halt_on_error=1").

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
