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

# Before any module subdirectory is configured: each module's CMakeLists probes
# for ReactAndroid::jsi to decide whether to compile its JSI registration edge.
find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)

# --- QuickJS modules ---------------------------------------------------------
#
# Installed modules (@react-native-quickjs/* declaring reactNativeQuickJSModule,
# listed by build.gradle as package directories) are folded into THIS single
# .so. Their core and their Android platform JNI compile into the engine image,
# linked whole-archive so the QJS_REGISTER_MODULE static initializer in each
# module's JSI object runs when the engine .so loads, and so the platform's
# exported Java_<module>_nativeAttach survives the link (nothing else references
# it). A module must not ship its own .so: it would load before the engine (the
# app instantiates packages before the runtime factory) and fail to resolve the
# engine's JS_* API.
#
# RNQJS_QUICKJS_FOLDED tells a module's platform JNI not to define its own
# JNI_OnLoad (only one may exist per .so) and to capture the JavaVM from the
# JNIEnv its attach call arrives with instead.
if(DEFINED RNQJS_QUICKJS_MODULES)
  set(REACT_NATIVE_QUICKJS_DIR "${RNQJS_ROOT}" CACHE PATH "" FORCE)
  add_compile_definitions(RNQJS_QUICKJS_FOLDED=1)

  set(RNQJS_FOLDED_TARGETS "")
  set(RNQJS_REGISTRY_DIRS "")
  foreach(MODULE_DIR IN LISTS RNQJS_QUICKJS_MODULES)
    get_filename_component(MODULE_NAME "${MODULE_DIR}" NAME)
    list(APPEND RNQJS_REGISTRY_DIRS "--dir" "${MODULE_DIR}")
    add_subdirectory("${MODULE_DIR}/android" "quickjs-modules/${MODULE_NAME}")
    if(NOT RNQJS_QUICKJS_MODULE_TARGETS)
      message(FATAL_ERROR
        "${MODULE_DIR}/android did not declare RNQJS_QUICKJS_MODULE_TARGETS "
        "(the static libraries to fold into the engine .so)")
    endif()
    list(APPEND RNQJS_FOLDED_TARGETS ${RNQJS_QUICKJS_MODULE_TARGETS})
    unset(RNQJS_QUICKJS_MODULE_TARGETS)
  endforeach()

  # The generated registry names each module's install function explicitly. The
  # reference makes a module that configured without its JSI edge a link error
  # rather than a silently missing global, and gives installs a deterministic
  # order. installModules() calls registerGeneratedModules(); QuickJSModule.cpp
  # declares it weak, and this strong definition in the same .so wins.
  set(RNQJS_REGISTRY "${CMAKE_CURRENT_BINARY_DIR}/generated/QuickJSGeneratedModules.cpp")
  find_program(RNQJS_NODE NAMES node REQUIRED)
  add_custom_command(
    OUTPUT "${RNQJS_REGISTRY}"
    COMMAND "${RNQJS_NODE}"
            "${RNQJS_ROOT}/scripts/generate-module-registry.js"
            ${RNQJS_REGISTRY_DIRS}
            --out "${RNQJS_REGISTRY}"
            --quiet
    DEPENDS
      "${RNQJS_ROOT}/scripts/generate-module-registry.js"
    COMMENT "Generating the QuickJS module registry"
    VERBATIM)
endif()

# Every src/ translation unit has to be listed. The Apple build globs src/, so
# a file added there builds on iOS and fails to link on Android -- an asymmetry
# worth remembering when adding a source.
set(RNQJS_QUICKJSINSTANCE_SOURCES
  "${RNQJS_ROOT}/src/bytecode/QuickJSBytecode.cpp"
  "${RNQJS_ROOT}/src/module/QuickJSCompat.cpp"
  "${RNQJS_ROOT}/src/module/QuickJSModule.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSInstance.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSRuntime.cpp"
  "${RNQJS_ROOT}/src/runtime/QuickJSRuntimeFactory.cpp"
  src/main/jni/JQuickJSInstance.cpp
  src/main/jni/OnLoad.cpp
)
if(DEFINED RNQJS_QUICKJS_MODULES)
  list(APPEND RNQJS_QUICKJSINSTANCE_SOURCES "${RNQJS_REGISTRY}")
endif()

add_library(quickjsinstancejni SHARED ${RNQJS_QUICKJSINSTANCE_SOURCES})

target_include_directories(quickjsinstancejni PRIVATE
  "${RNQJS_ROOT}/src/bytecode"
  "${RNQJS_ROOT}/src/module"
  "${RNQJS_ROOT}/src/runtime"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main/jni"
)

# The module archives and the engine's own quickjs static archive are grouped so
# the linker resolves references in both directions no matter their order. The
# modules are whole-archived: a static module's platform object is referenced
# only by a name Java resolves at runtime, and its QJS_REGISTER_MODULE
# initializer must run at load, so every object has to be in.
if(RNQJS_FOLDED_TARGETS)
  target_link_libraries(quickjsinstancejni
    -Wl,--start-group
    -Wl,--whole-archive
    ${RNQJS_FOLDED_TARGETS}
    -Wl,--no-whole-archive
    quickjs
    -Wl,--end-group
    fbjni::fbjni
    ReactAndroid::jsi
    ReactAndroid::reactnative
    android
    log
  )
else()
  target_link_libraries(quickjsinstancejni
    quickjs
    fbjni::fbjni
    ReactAndroid::jsi
    ReactAndroid::reactnative
    android
    log
  )
endif()

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
# On by default: react-native-worklets does not load without it, and neither
# does react-native-reanimated, which is most apps. -PrnqjsHermesCompat=false
# turns it off.
option(RNQJS_HERMES_COMPAT "Build the Hermes compatibility shim as libhermesvm.so" ON)
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
