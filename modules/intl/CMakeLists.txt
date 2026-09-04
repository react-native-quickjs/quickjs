# react-native-quickjs-intl
#
# Shared by the Android and Apple builds. Compiles from source against the
# QuickJS engine the app already builds — nothing here is ever prebuilt.

cmake_minimum_required(VERSION 3.20)

set(RNQJS_MODULE_NAME react-native-quickjs-intl)
# ...and in the caller's scope too: android/CMakeLists.txt links
# ${RNQJS_MODULE_NAME} after add_subdirectory(), and without this the variable
# is empty there, so the JNI library links without the module and fails with
# `undefined symbol: rnqjs::intl::setPlatform`.
set(RNQJS_MODULE_NAME "${RNQJS_MODULE_NAME}" PARENT_SCOPE)

if(NOT DEFINED REACT_NATIVE_QUICKJS_DIR)
  set(REACT_NATIVE_QUICKJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs"
      CACHE PATH "Path to the react-native-quickjs package")
endif()

if(NOT EXISTS "${REACT_NATIVE_QUICKJS_DIR}/src/module/QuickJSModule.h")
  message(FATAL_ERROR
    "Could not find react-native-quickjs at ${REACT_NATIVE_QUICKJS_DIR}. "
    "Pass -DREACT_NATIVE_QUICKJS_DIR=/path/to/react-native-quickjs")
endif()

set(REACT_NATIVE_QUICKJS_DIR "${REACT_NATIVE_QUICKJS_DIR}" PARENT_SCOPE)

# --- The bytecode blob ------------------------------------------------------
#
# js/intl.js is compiled to QuickJS bytecode at build time and embedded as a
# byte array. The compiler is `qjsc` built from the *same* engine as the
# runtime: bytecode is loadable only by the engine build that produced it. An
# Android or iOS build cross-compiles and cannot run the qjsc it builds for the
# device, so a host compiler is used instead:
#
#   1. -DRNQJS_INTL_QJSC=/path/to/host/qjsc   (explicit)
#   2. this repository's `qjsc` target when building in-tree
#   3. the engine package's committed per-host compiler, bin/qjsc/<os>-<arch>/qjsc
#   4. none — the module embeds the *source* and compiles it on first use of
#      Intl. Correct and still lazy, but it pays the parse.
#
# All three build systems (host CMake, the Android NDK build, the iOS pod)
# reach this same CMakeLists or the same embed-js.js, so the discovery lives
# here alone.

if(NOT DEFINED RNQJS_INTL_QJSC)
  set(RNQJS_INTL_QJSC "" CACHE FILEPATH "Host qjsc for the Intl bytecode blob")
endif()

if(NOT RNQJS_INTL_QJSC AND TARGET qjsc)
  set(RNQJS_INTL_QJSC "${CMAKE_BINARY_DIR}/qjsc")
endif()

if(NOT RNQJS_INTL_QJSC)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_rnqjs_host "darwin")
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_rnqjs_host "linux")
  endif()
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(_rnqjs_arch "x64")
  elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_rnqjs_arch "arm64")
  endif()
  if(_rnqjs_host AND _rnqjs_arch)
    set(_rnqjs_qjsc
        "${REACT_NATIVE_QUICKJS_DIR}/bin/qjsc/${_rnqjs_host}-${_rnqjs_arch}/qjsc")
    if(EXISTS "${_rnqjs_qjsc}")
      set(RNQJS_INTL_QJSC "${_rnqjs_qjsc}")
    endif()
  endif()
endif()

find_program(RNQJS_NODE NAMES node REQUIRED)

set(RNQJS_INTL_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/intl-generated")

set(RNQJS_INTL_EMBED_ARGS --out "${RNQJS_INTL_GENERATED}")
if(RNQJS_INTL_QJSC)
  list(APPEND RNQJS_INTL_EMBED_ARGS --qjsc "${RNQJS_INTL_QJSC}")
  message(STATUS "intl: precompiling js/intl.js with ${RNQJS_INTL_QJSC}")
else()
  message(WARNING
    "intl: no host qjsc found; the Intl JavaScript layer will be embedded as "
    "source and compiled on first use of Intl. Pass -DRNQJS_INTL_QJSC=<path> "
    "to a qjsc built from this same engine revision to avoid the parse.")
endif()

# The size gates live in scripts/embed-js.js, so they fire here as a build
# failure: source and blob budgets, enforced next to the numbers and the
# reasoning that set them.
add_custom_command(
  OUTPUT
    "${RNQJS_INTL_GENERATED}/IntlSource.h"
    "${RNQJS_INTL_GENERATED}/IntlBlob.h"
    "${RNQJS_INTL_GENERATED}/IntlBuildConfig.h"
  COMMAND "${RNQJS_NODE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-js.js"
          ${RNQJS_INTL_EMBED_ARGS}
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/js/intl.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-js.js"
  COMMENT "intl: embedding js/intl.js (size gates enforced by scripts/embed-js.js)"
  VERBATIM)

add_custom_target(rnqjs_intl_generate
  DEPENDS
    "${RNQJS_INTL_GENERATED}/IntlSource.h"
    "${RNQJS_INTL_GENERATED}/IntlBlob.h"
    "${RNQJS_INTL_GENERATED}/IntlBuildConfig.h")

if(RNQJS_INTL_QJSC AND TARGET qjsc)
  add_dependencies(rnqjs_intl_generate qjsc)
endif()
# --- The JSI edge -----------------------------------------------------------
#
# cpp/IntlModuleJSI.cpp is the only file that includes a JSI header, and JSI is
# absent from the host CLI and the Android NDK backend builds. The edge is
# conditional so those builds still work, and loud rather than silent: without
# JSI there is no QJS_REGISTER_MODULE, so an app gets no Intl — the STATUS line
# says exactly that.

set(RNQJS_INTL_JSI_INCLUDE "")
set(RNQJS_INTL_JSI_TARGET "")
find_package(ReactAndroid CONFIG QUIET)
if(TARGET ReactAndroid::jsi)
  set(RNQJS_INTL_JSI_TARGET ReactAndroid::jsi)
elseif(TARGET jsi)
  set(RNQJS_INTL_JSI_TARGET jsi)
elseif(EXISTS "${REACT_NATIVE_QUICKJS_DIR}/node_modules/react-native/ReactCommon/jsi/jsi/jsi.h")
  set(RNQJS_INTL_JSI_INCLUDE
      "${REACT_NATIVE_QUICKJS_DIR}/node_modules/react-native/ReactCommon/jsi")
endif()

# --- The module -------------------------------------------------------------

set(RNQJS_INTL_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/IntlModule.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/IntlPlatform.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/IntlNumberParts.cpp
)

set(RNQJS_INTL_SOURCES ${RNQJS_INTL_CORE_SOURCES})
if(RNQJS_INTL_JSI_INCLUDE OR RNQJS_INTL_JSI_TARGET)
  list(APPEND RNQJS_INTL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cpp/IntlModuleJSI.cpp)
else()
  message(STATUS
    "intl: no JSI headers found; building without cpp/IntlModuleJSI.cpp. "
    "The Intl implementation is complete, but QJS_REGISTER_MODULE and "
    "intl_install() are NOT compiled in, so a React Native app built this way "
    "will have no Intl. Pass -DRNQJS_JSI_INCLUDE_DIR=<dir containing jsi/jsi.h>.")
endif()

add_library(${RNQJS_MODULE_NAME} STATIC ${RNQJS_INTL_SOURCES})

add_dependencies(${RNQJS_MODULE_NAME} rnqjs_intl_generate)

target_include_directories(${RNQJS_MODULE_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp
  ${RNQJS_INTL_GENERATED}
  ${REACT_NATIVE_QUICKJS_DIR}/src/bytecode
  ${REACT_NATIVE_QUICKJS_DIR}/src/module
  ${REACT_NATIVE_QUICKJS_DIR}/src/runtime
  ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel
)
if(RNQJS_INTL_JSI_INCLUDE)
  target_include_directories(${RNQJS_MODULE_NAME} PUBLIC ${RNQJS_INTL_JSI_INCLUDE})
endif()
if(RNQJS_INTL_JSI_TARGET)
  target_link_libraries(${RNQJS_MODULE_NAME} PUBLIC ${RNQJS_INTL_JSI_TARGET})
endif()

target_compile_features(${RNQJS_MODULE_NAME} PUBLIC cxx_std_20)
set_target_properties(${RNQJS_MODULE_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Host tests -------------------------------------------------------------
#
# `intl_tests` links the module with the default, no-platform backend (a
# working en-US stub) and runs test/invariants.js, the module's own
# backend-agnostic self-checks. This is a real engine+module test on every
# host, including CI's Linux runners.

if(TARGET quickjs_jsi AND TARGET GTest::gtest)
  target_link_libraries(${RNQJS_MODULE_NAME} PUBLIC quickjs_jsi)

  add_executable(intl_tests test/intl_test.cpp)
  target_link_libraries(intl_tests
    PRIVATE ${RNQJS_MODULE_NAME} GTest::gtest GTest::gtest_main)
  target_compile_definitions(intl_tests PRIVATE
    RNQJS_INTL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test")
  set_target_properties(intl_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  add_test(NAME intl COMMAND intl_tests)
endif()

# --- Host tooling -----------------------------------------------------------
#
# `intl-cli` is a QuickJS shell with this module installed and no React Native
# in the link, for running the module's JS checks and the bench suite against
# node without a device. It links the default backend directly, like the tests.
#
# Off by default because it compiles the module core a second time for a shell
# an app build does not need.

option(RNQJS_INTL_BUILD_CLI "Build the host intl-cli shell" OFF)
if(RNQJS_INTL_BUILD_CLI)
  add_executable(intl-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/intl-cli.c
    ${RNQJS_INTL_CORE_SOURCES})
  add_dependencies(intl-cli rnqjs_intl_generate)
  target_include_directories(intl-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp
    ${RNQJS_INTL_GENERATED}
    ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel)
  target_compile_features(intl-cli PRIVATE cxx_std_20)
  target_link_libraries(intl-cli quickjs)

  add_executable(intl-bench
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/intl-bench.c
    ${RNQJS_INTL_CORE_SOURCES})
  add_dependencies(intl-bench rnqjs_intl_generate)
  target_include_directories(intl-bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp
    ${RNQJS_INTL_GENERATED}
    ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel)
  target_compile_features(intl-bench PRIVATE cxx_std_20)
  target_link_libraries(intl-bench quickjs)

  # intl-cli-apple — the same shell with the Apple backend linked. The backend
  # is chosen at link time (ios/IntlPlatform.mm registers itself from +load;
  # the stub is what you get when it is absent), and --print-backend says which
  # one answered.
  if(APPLE)
    option(RNQJS_INTL_WITH_SWIFT "Link ios/IntlLikelySubtags.swift" ON)

    set(RNQJS_INTL_APPLE_EXTRA "")
    if(RNQJS_INTL_WITH_SWIFT)
      find_program(RNQJS_SWIFTC NAMES swiftc)
      if(RNQJS_SWIFTC)
        set(RNQJS_INTL_SWIFT_OBJ "${CMAKE_CURRENT_BINARY_DIR}/IntlLikelySubtags.o")
        add_custom_command(
          OUTPUT "${RNQJS_INTL_SWIFT_OBJ}"
          COMMAND "${RNQJS_SWIFTC}" -O -parse-as-library -emit-object
                  -o "${RNQJS_INTL_SWIFT_OBJ}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/ios/IntlLikelySubtags.swift"
          DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ios/IntlLikelySubtags.swift"
          COMMENT "intl: compiling IntlLikelySubtags.swift"
          VERBATIM)
        set(RNQJS_INTL_APPLE_EXTRA "${RNQJS_INTL_SWIFT_OBJ}")
      else()
        message(WARNING "intl: no swiftc; intl-cli-apple will use the weak-symbol "
                        "fallback and report no opinion on likely subtags")
      endif()
    endif()

    add_executable(intl-cli-apple
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/intl-cli.c
      ${RNQJS_INTL_CORE_SOURCES}
      ${CMAKE_CURRENT_SOURCE_DIR}/ios/IntlPlatform.mm
      ${RNQJS_INTL_APPLE_EXTRA})
    add_dependencies(intl-cli-apple rnqjs_intl_generate)
    target_include_directories(intl-cli-apple PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/cpp
      ${RNQJS_INTL_GENERATED}
      ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel)
    target_compile_features(intl-cli-apple PRIVATE cxx_std_20)
    # ARC, because that is what the pod gets: CocoaPods defaults s.requires_arc
    # to true, so a non-ARC host build would measure a different program than
    # the one that ships.
    set_source_files_properties(
      ${CMAKE_CURRENT_SOURCE_DIR}/ios/IntlPlatform.mm
      PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(intl-cli-apple quickjs "-framework Foundation")
  endif()
endif()
