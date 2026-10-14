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

# The experimental register VM (`QJS_REGVM=1`), off by default.
#
# One binary contains one VM, so this is a build-wide fork rather than a runtime
# switch: build one variant per build directory, from this same source tree.
#
#   cmake -B build            # stack VM (default)
#   cmake -B build-regvm -DQJS_REGVM=ON   # register VM
#
# When OFF the flag defines nothing at all, so the compile command line -- and
# therefore the objects and binaries -- are exactly what they were before this
# option existed; adding a `QJS_REGVM=0` define in the off case would put the
# flag into every compile line for no benefit. The engine's `#if QJS_REGVM`
# reads an undefined macro as 0.
option(QJS_REGVM "Build the experimental register VM engine" OFF)

if(QJS_REGVM)
  # A register build compiles the patched submodule directly.
  set(QUICKJS_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine/quickjs-ng")
else()
  set(QUICKJS_DIR "${CMAKE_CURRENT_LIST_DIR}/../engine/quickjs-rel")
endif()

if(NOT EXISTS "${QUICKJS_DIR}/quickjs.c")
  message(FATAL_ERROR
    "${QUICKJS_DIR} is missing.\n"
    "Run: node scripts/sync-quickjs-rel.js")
endif()

# Patch development edits the submodule in place, but the shipping target
# compiles the projection, so an unsynced edit would be silently ignored. In a
# source checkout -- never in a consumer's node_modules -- fail instead, and name
# the command that fixes it. A register build compiles the submodule directly,
# so the projection's freshness is not what it is testing.
if(NOT QJS_REGVM
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../engine/quickjs-ng/quickjs.c"
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

if(QJS_REGVM)
  # Production packaging stays on the stack VM for now. Failing here is the
  # point: a register-VM Android or iOS artifact would be unusable if it were
  # shipped by accident, because its bytecode is not loadable by the shipping
  # engine and vice versa.
  if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR
      "QJS_REGVM=ON is not supported for Android or iOS yet.\n"
      "Production builds are stack-only for now; host builds may set "
      "QJS_REGVM=ON freely.")
  endif()
  target_compile_definitions(quickjs PUBLIC QJS_REGVM=1)

  # The register VM's default executor is the threaded dispatch plus the record
  # call path; the engine's header defaults turn both on for a Clang register
  # build (see `engine/quickjs-ng/quickjs.c`). They are exposed here so a
  # configure can opt back into the monolith, which the AOT runtime and the AOT
  # profiler require -- with `QJS_AOT=ON` the engine keeps them off by itself.
  option(QJS_REGVM_THREAD "Threaded dispatch for the register VM" ON)
  option(QJS_REGVM_RECORD "Record call path for the register VM" ON)
  if(NOT QJS_REGVM_THREAD)
    target_compile_definitions(quickjs PUBLIC QJS_REGVM_THREAD=0)
  endif()
  if(NOT QJS_REGVM_RECORD)
    target_compile_definitions(quickjs PUBLIC QJS_REGVM_RECORD=0)
  endif()
endif()

# Ahead-of-time compilation of the register VM (`QJS_AOT=1`). The generator
# (`tools/aot/qjsaot`) turns a profiled bundle into one C translation unit of
# straight-line `qjs_aot_step` calls, which is linked into the engine and bound
# lazily by bytecode digest. QJS_AOT_UNIT names that generated unit; with no
# unit the engine still builds (the table is a weak symbol) and simply runs
# everything through the interpreter.
option(QJS_AOT "Build the register VM with an AOT unit" OFF)
set(QJS_AOT_UNIT "" CACHE FILEPATH "Generated AOT unit (aot_unit.c) to link into quickjs")
if(QJS_AOT)
  if(NOT QJS_REGVM)
    message(FATAL_ERROR "QJS_AOT=ON requires QJS_REGVM=ON")
  endif()
  target_compile_definitions(quickjs PUBLIC QJS_AOT=1)
  if(QJS_AOT_UNIT)
    if(NOT EXISTS "${QJS_AOT_UNIT}")
      message(FATAL_ERROR "QJS_AOT_UNIT does not exist: ${QJS_AOT_UNIT}")
    endif()
    target_sources(quickjs PRIVATE "${QJS_AOT_UNIT}")
  endif()
endif()

# Lazy JSON is opt-in for applications. Android and CocoaPods derive this
# option from the consuming app's package.json; host builds can override it
# with -DRNQJS_JSON_LAZY_DEFAULT=ON or QJS_JSON_LAZY at runtime for tests.
option(RNQJS_JSON_LAZY_DEFAULT
       "Enable lazy JSON parsing by default for this build" OFF)
if(RNQJS_JSON_LAZY_DEFAULT)
  target_compile_definitions(quickjs PRIVATE QJS_JSON_LAZY_DEFAULT=1)
endif()

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

# The interpreter recurses on the native stack: one JavaScript call is one
# JS_CallInternal frame. Unoptimized that frame is around 10 KB, so on React
# Native's roughly 1 MB JavaScript thread only about 80 JavaScript calls fit --
# far too few for React to render, and a debug app comes up blank with nothing
# in the log but "Maximum call stack size exceeded". Optimized the same frame is
# a small fraction of that.
#
# So an app build compiles the engine optimized whatever the app itself is
# built as. It is a dependency rather than the code an app author steps
# through, and -g is kept, so a stack trace through it still resolves.
#
# Host builds are left alone: they are this project's own tests, they run on a
# far larger stack than a React Native JavaScript thread, and the third-party
# JSI conformance suite measures stack-overflow behaviour that this changes.
if(ANDROID AND NOT MSVC)
  target_compile_options(quickjs PRIVATE $<$<CONFIG:Debug>:-O2>)
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

# x86: keep branches off 32-byte boundaries. On Skylake-family cores (JCC
# erratum) handler placement alone moved Octane rows by up to 16% at identical
# instruction counts.
include(CheckCCompilerFlag)
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  # clang takes the flag directly; GCC passes it to the GNU assembler.
  check_c_compiler_flag(-mbranches-within-32B-boundaries QJS_HAVE_BRANCH_ALIGN)
  check_c_compiler_flag(-Wa,-mbranches-within-32B-boundaries QJS_HAVE_AS_BRANCH_ALIGN)
  if(QJS_HAVE_BRANCH_ALIGN)
    target_compile_options(quickjs PRIVATE -mbranches-within-32B-boundaries)
  elseif(QJS_HAVE_AS_BRANCH_ALIGN)
    target_compile_options(quickjs PRIVATE -Wa,-mbranches-within-32B-boundaries)
  endif()
endif()
