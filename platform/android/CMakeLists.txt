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

set(RNQJS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

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
