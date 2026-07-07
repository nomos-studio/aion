# nomos-studio C++ review instructions

These instructions apply to all C++ code in this repository.
Follow them on every pull request review.

---

## Thread safety — audio thread constraints

This codebase has a hard real-time audio thread. The following are
**absolute rules** — flag any violation as a bug, not a suggestion:

- **No heap allocation on the audio thread.** `new`, `delete`,
  `std::make_unique`, `std::make_shared`, `std::vector::push_back`
  (when it may reallocate), `std::string` construction from literals —
  all forbidden in any code path reachable from the audio callback.
- **No blocking on the audio thread.** No mutex lock, no `sleep`, no
  `std::condition_variable::wait`, no file I/O, no system calls that
  may block.
- **`rcu_managed<T>::store()` is control-thread-only.** The `store()`
  method allocates a `retire_node` internally. Calling it from the audio
  thread is a realtime violation. The audio thread may only call
  `rcu_managed<T>::read()`, which returns a `reader_guard` RAII object.
- **`spsc_queue` has one producer and one consumer.** Do not call the
  producer-side methods from the consumer thread or vice versa.

---

## RCU usage (`nomos/rt/rcu.hpp`)

`rcu_managed<T>` is the approved mechanism for sharing state between the
control thread (writer) and the audio thread (reader).

Correct usage:
```cpp
// Control thread — writer
managed.store(std::make_unique<T>(...));

// Audio thread — reader
auto guard = managed.read();   // RAII, holds read-side lock
if (guard) guard->method();
```

Flag any code that:
- Calls `store()` from a non-control thread.
- Reads `ptr_` directly (bypassing `read()`) in production code outside
  of single-threaded tests.
- Stores a non-standard-layout type in `rcu_managed<T>` — the retire
  callback uses `caa_container_of` / `offsetof`, which requires
  `retire_node` (internal to `rcu_managed`) to be standard-layout.
  If `T` itself has a non-trivial destructor that is not safe to call
  from the RCU callback thread, flag that too.

---

## Shift operations — undefined behaviour

Shifting a value by an amount equal to or greater than the type width is
undefined behaviour in C++. A confirmed instance was fixed at `b36ff8d`
(`shift_register_modulator.cpp`). Always verify:

```cpp
// Wrong — UB when n == 32
(1u << n) - 1u

// Correct
n < 32 ? ((1u << n) - 1u) : 0xFFFF'FFFFu
```

Flag any `<<` or `>>` where the shift amount is a variable (especially
one sourced from EDN / user input) without a guard that ensures it is
strictly less than the type width.

---

## CMake — sanitizer wiring

This codebase uses `cmake/Sanitizers.cmake` which provides two APIs:

- **`sanitizers::sanitizers`** — INTERFACE target carrying both compile
  and link flags. Use this on **test executables** and non-exported
  binaries only.
- **`nomos_sanitize_target(<target>)`** — applies compile flags only,
  directly via `target_compile_options`. Use this on **exported static
  libraries**. Never use `target_link_libraries(<lib> PRIVATE
  sanitizers::sanitizers)` on an exported target — it creates a
  dependency on a non-exported INTERFACE target and breaks
  `install(EXPORT ...)`.

Flag any new target that:
- Is an exported library and uses `sanitizers::sanitizers` directly.
- Is a test executable and does not link `sanitizers::sanitizers`.

---

## CMake — FetchContent and dep overrides

Every `FetchContent_Declare` call must:
1. Use an **HTTPS** URL (`https://github.com/...`), never SSH
   (`git@github.com:...`). SSH requires a deploy key in CI runners.
2. Be guarded by an `if(DEFINED <DEP>_DIR)` block that substitutes
   `SOURCE_DIR "${<DEP>_DIR}"` when the variable is set, allowing
   local and CI overrides without network access.

```cmake
if(DEFINED FOO_DIR)
    FetchContent_Declare(foo SOURCE_DIR "${FOO_DIR}")
else()
    FetchContent_Declare(foo
        GIT_REPOSITORY https://github.com/nomos-studio/foo.git
        GIT_TAG        <sha>
        GIT_SHALLOW    TRUE)
endif()
```

Flag any `FetchContent_Declare` that uses SSH or lacks the `SOURCE_DIR`
override guard.

---

## REUSE / SPDX compliance

Every new source file must carry both of these in the first comment block:

```
// SPDX-License-Identifier: <licence>
// SPDX-FileCopyrightText: <year> nomos-studio contributors
```

Correct licences by repo:
- `edn-cpp`, `txlog`, `nomos-topology`: `BSL-1.0`
- `nomos-rt`, `kairos`, `aion`: `GPL-2.0-or-later`
- `kairos-grid`: `GPL-3.0-or-later`

Flag any new `.cpp`, `.hpp`, or `CMakeLists.txt` file missing either tag.

---

## C++ standard — C++20 minimum

All targets in this codebase build with `CXX_STANDARD 20`. Do not:

- Use `#ifdef` guards or polyfills to work around missing C++20 features.
- Lower the standard for any target or dependency (no
  `set(CMAKE_CXX_STANDARD 17)` anywhere in the tree).
- Use C++23 features without an `#if __cplusplus >= 202302L` guard and
  a C++20 fallback.

---

## IPC framing (nomos-rt, aion)

nomos-rt and aion receive EDN frames from nous (the Clojure supervisor)
over stdin. Flag any message-handling code path where:

- A missing required EDN key is silently ignored rather than returning
  an error to the caller.
- A value with an unexpected type (string where keyword expected, nil
  where map expected) causes undefined behaviour or silent state
  corruption rather than a logged error.
- The frame-reading loop does not handle truncated input (EOF
  mid-frame) cleanly.
