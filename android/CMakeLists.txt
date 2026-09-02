# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Builds libquickjsinstancejni.so, the shared library the Kotlin QuickJSInstance
# loads. Driven by build.gradle, never configured directly.

cmake_minimum_required(VERSION 3.20)
project(quickjsinstancejni CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED REACT_NATIVE_DIR)
  message(FATAL_ERROR "REACT_NATIVE_DIR must be passed in from build.gradle")
endif()

set(RNQJS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# React Native's headers reach into folly, which does not compile without these
# flags unless a generated folly-config.h is on the include path.
include("${REACT_NATIVE_DIR}/ReactAndroid/cmake-utils/folly-flags.cmake")
add_compile_options(${folly_FLAGS})

include("${RNQJS_ROOT}/cmake/quickjs.cmake")

# Every src/ translation unit has to be listed. The Apple build globs src/,
# so a file added there builds on iOS and fails to link on Android -- an
# asymmetry worth remembering when adding a source.
add_library(quickjsinstancejni SHARED
  "${RNQJS_ROOT}/src/bytecode/QuickJSBytecode.cpp"
  "${RNQJS_ROOT}/src/module/QuickJSCompat.cpp"
  "${RNQJS_ROOT}/src/module/QuickJSModule.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSInstance.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSRuntime.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSRuntimeFactory.cpp"
  src/main/jni/JQuickJSInstance.cpp
  src/main/jni/OnLoad.cpp
)

target_include_directories(quickjsinstancejni PRIVATE
  "${RNQJS_ROOT}/src/bytecode"
  "${RNQJS_ROOT}/src/module"
  "${RNQJS_ROOT}/src/runtime"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main/jni"
)

find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

target_link_libraries(quickjsinstancejni
  quickjs
  fbjni::fbjni
  ReactAndroid::jsi
  ReactAndroid::reactnative
  android
  log
)

# --- Chrome DevTools Protocol ----------------------------------------------
#
# In debug builds, out of release builds, which is what
# QuickJSInstance::debuggerEnabledByDefault() already decides at runtime.
# -DRNQJS_ENABLE_CDP=ON puts it back.
#
# The two sources are compiled here rather than through modules/cdp's own
# CMakeLists because that file also declares a host test target. Two files is
# less machinery than teaching it to be cross-compiled.
if(CMAKE_BUILD_TYPE MATCHES Debug)
  set(RNQJS_CDP_DEFAULT ON)
else()
  set(RNQJS_CDP_DEFAULT OFF)
endif()
option(RNQJS_ENABLE_CDP "Compile in the Chrome DevTools Protocol backend"
       ${RNQJS_CDP_DEFAULT})

if(RNQJS_ENABLE_CDP)
  set(CDP_DIR "${RNQJS_ROOT}/modules/cdp")
  target_sources(quickjsinstancejni PRIVATE
    "${CDP_DIR}/quickjs-cdp.c"
    "${CDP_DIR}/react/QuickJSInspector.cpp"
  )
  target_include_directories(quickjsinstancejni PRIVATE
    "${CDP_DIR}" "${CDP_DIR}/react")
  target_compile_definitions(quickjsinstancejni PRIVATE RNQJS_ENABLE_CDP=1)
endif()

# The Hermes compatibility shim, opt-in, built as libhermesvm.so.
#
# react-native-worklets picks its engine at compile time and links React
# Native's imported `hermes` target, so what it records as NEEDED is the file
# that target points at. On React Native 0.85 that is libhermesvm.so, not
# libhermes.so -- see ReactAndroid/src/main/jni/first-party/hermes/CMakeLists.txt.
# Giving it a library of that name, whose makeHermesRuntime returns a QuickJS
# runtime, is what lets it run here unmodified.
#
# It links libquickjsinstancejni.so rather than compiling the engine again:
# two copies would mean two engines in one app, and roughly a megabyte of
# duplicate code per architecture.
#
# Off by default because a library named libhermesvm.so changes what every
# Hermes-detecting library in the app believes. -PrnqjsHermesCompat=true.
option(RNQJS_HERMES_COMPAT "Build the Hermes compatibility shim as libhermesvm.so" OFF)
if(RNQJS_HERMES_COMPAT)
  add_library(hermes SHARED
    "${RNQJS_ROOT}/modules/hermes-compat/src/HermesCompat.cpp")

  target_include_directories(hermes PUBLIC
    "${RNQJS_ROOT}/modules/hermes-compat/include")
  target_include_directories(hermes PRIVATE
    "${RNQJS_ROOT}/src/bytecode"
    "${RNQJS_ROOT}/src/module"
    "${RNQJS_ROOT}/src/runtime")

  # Unconditional, matching the podspec: RNWorklets defines this for every
  # Debug build, so enableDebugging must exist whatever this library was
  # compiled with. Safe only because no member declaration in the shim's
  # headers is conditional on it.
  target_compile_definitions(hermes PRIVATE HERMES_ENABLE_DEBUGGER=1)

  target_link_libraries(hermes
    quickjsinstancejni
    ReactAndroid::jsi
    android
    log
  )

  set_target_properties(hermes PROPERTIES
    OUTPUT_NAME hermesvm
    POSITION_INDEPENDENT_CODE ON
  )
endif()
