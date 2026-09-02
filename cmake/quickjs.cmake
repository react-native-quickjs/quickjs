# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Defines the `quickjs` static library.
#
# It compiles engine/quickjs-rel, the committed pre-patched copy, not the
# engine/quickjs-ng submodule -- see the header of scripts/sync-quickjs-rel.js
# for why the submodule cannot be the thing that ships.
#
# Upstream's own CMakeLists is deliberately not used: it also declares qjs,
# qjsc and api-test, none of which link for Android or iOS, and we want direct
# control over the compile flags. The source list below mirrors upstream's
# `qjs_sources`; keep it in sync when bumping the submodule.

set(QUICKJS_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine/quickjs-rel")

if(NOT EXISTS "${QUICKJS_DIR}/quickjs.c")
  message(FATAL_ERROR
    "engine/quickjs-rel is missing.\n"
    "Run: node scripts/sync-quickjs-rel.js")
endif()

# Patch development edits the submodule in place, but this target compiles the
# projection, so an unsynced edit would be silently ignored. In a source
# checkout -- never in a consumer's node_modules -- fail instead, and name the
# command that fixes it.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../engine/quickjs-ng/quickjs.c"
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../scripts/sync-quickjs-rel.js")
  find_program(QUICKJS_NODE_EXECUTABLE node)
  if(QUICKJS_NODE_EXECUTABLE)
    execute_process(
      COMMAND "${QUICKJS_NODE_EXECUTABLE}"
              "${CMAKE_CURRENT_LIST_DIR}/../scripts/sync-quickjs-rel.js" --check
      RESULT_VARIABLE QUICKJS_REL_STALE
      OUTPUT_QUIET
      ERROR_VARIABLE QUICKJS_REL_ERROR)
    if(NOT QUICKJS_REL_STALE EQUAL 0)
      message(FATAL_ERROR "${QUICKJS_REL_ERROR}")
    endif()
  else()
    message(STATUS
      "quickjs: node not found, skipping the projection staleness check. "
      "Engine edits made in engine/quickjs-ng will NOT be compiled until you "
      "run scripts/sync-quickjs-rel.js.")
  endif()
endif()

add_library(quickjs STATIC
  "${QUICKJS_DIR}/quickjs.c"
  "${QUICKJS_DIR}/libregexp.c"
  "${QUICKJS_DIR}/libunicode.c"
  "${QUICKJS_DIR}/dtoa.c"
)

target_include_directories(quickjs PUBLIC "${QUICKJS_DIR}")
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)

# The maths functions are in libSystem on Apple and in a separate libm
# elsewhere, so a macOS host build links clean while Linux fails at the link
# step on round, floor, pow and friends. Detected rather than assumed, the way
# quickjs-ng's own CMakeLists does it.
find_library(QUICKJS_M_LIBRARY m)
if(QUICKJS_M_LIBRARY)
  target_link_libraries(quickjs PUBLIC m)
endif()

find_package(Threads)
target_link_libraries(quickjs PUBLIC ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})

# The debugger interface added by engine/patches/0003. It costs nothing at
# runtime with no trace handler installed, because the per-statement traps are
# emitted at parse time only when there is one. PUBLIC because embedders
# calling the JS_* debugger entry points must see the value the engine was
# built with.
option(QUICKJS_ENABLE_DEBUGGER "Compile in the engine-level debugger interface" ON)
if(QUICKJS_ENABLE_DEBUGGER)
  target_compile_definitions(quickjs PUBLIC JS_ENABLE_DEBUGGER=1)
else()
  target_compile_definitions(quickjs PUBLIC JS_ENABLE_DEBUGGER=0)
endif()

set_target_properties(quickjs PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

# Upstream builds cleanly, but not warning-free under React Native's flags.
# MSVC rejects these outright -- `cl` reads -Wn as the numeric warning level
# and stops with D8021 -- and it is only reached by the bytecode compiler,
# which is built for Windows too.
if(MSVC)
  # engine/patches/0005 stores the debug-trace arming flag in an `_Atomic bool`,
  # because it is written from a thread other than the one running JavaScript.
  # MSVC treats _Atomic as C11 it does not implement by default, and the first
  # error cascades into a hundred more. quickjs-ng passes the same flag.
  target_compile_options(quickjs PRIVATE /experimental:c11atomics)
else()
  target_compile_options(quickjs PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-implicit-fallthrough
  )
endif()
