# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# The host build, used for development and the test suites. Android builds
# through android/ and Apple through the podspec; all three reuse
# cmake/quickjs.cmake and the same src/ sources.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(react-native-quickjs C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Must come before any add_subdirectory() that registers a test. CMake drops
# add_test() calls made in directories processed before this line, with no
# warning -- the test simply never appears in `ctest -N`.
enable_testing()

include(cmake/quickjs.cmake)
include(cmake/jsi.cmake)

add_library(quickjs_jsi STATIC
  src/bytecode/QuickJSBytecode.cpp
  src/module/QuickJSCompat.cpp
  src/module/QuickJSModule.cpp
  src/runtime/QuickJSRuntime.cpp
  src/runtime/QuickJSRuntimeFactory.cpp
)
target_include_directories(quickjs_jsi PUBLIC src/bytecode src/module src/runtime)
target_link_libraries(quickjs_jsi PUBLIC jsi quickjs)
set_target_properties(quickjs_jsi PROPERTIES POSITION_INDEPENDENT_CODE ON)

# src/runtime/QuickJSInstance.cpp is deliberately not in quickjs_jsi. It
# includes react/runtime/JSRuntimeFactory.h, which reaches folly and
# jsinspector-modern -- both of which arrive through gradle and CocoaPods, not
# node_modules. The Android and Apple builds compile it; the host build cannot.

# Built from the engine in this tree, not shipped prebuilt: bytecode is only
# loadable by the exact engine build that produced it.
add_executable(qjsc tools/bytecode/qjsc.c)
target_link_libraries(qjsc PRIVATE quickjs)

add_executable(qjs-bench tools/benchmark/qjs-bench.c)
target_link_libraries(qjs-bench PRIVATE quickjs)
target_include_directories(qjs-bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}/engine/quickjs-rel")

add_executable(native-call-bench tools/benchmark/calls.c)
target_link_libraries(native-call-bench PRIVATE quickjs)
target_include_directories(native-call-bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}/engine/quickjs-rel")

add_subdirectory(tests)

# Added after tests/ on purpose: tests/CMakeLists.txt is where googletest is
# fetched, and modules/cdp registers its own test against GTest::gtest.
add_subdirectory(modules/cdp)
add_subdirectory(modules/hermes-compat)
set(REACT_NATIVE_QUICKJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
add_subdirectory(modules/text-encoding)
add_subdirectory(modules/intl)

# Test the generated module registry end to end: the TU must link the modules
# in without any test calling their install functions directly -- the host
# analogue of an app's iOS build. --dir keeps the list authoritative.
find_program(RNQJS_NODE_EXECUTABLE node)
if(RNQJS_NODE_EXECUTABLE AND TARGET GTest::gtest
   AND TARGET react-native-quickjs-intl
   AND TARGET react-native-quickjs-text-encoding)
  set(RNQJS_GENERATED_REGISTRY "${CMAKE_BINARY_DIR}/QuickJSGeneratedModules.cpp")
  add_custom_command(
    OUTPUT "${RNQJS_GENERATED_REGISTRY}"
    COMMAND "${RNQJS_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-module-registry.js"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/modules/intl"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/modules/text-encoding"
            --out "${RNQJS_GENERATED_REGISTRY}"
            --quiet
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-module-registry.js"
      "${CMAKE_CURRENT_SOURCE_DIR}/modules/intl/package.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/modules/text-encoding/package.json"
    COMMENT "Generating the QuickJS module registry"
    VERBATIM)

  add_executable(quickjs_generated_modules_tests
    tests/generated_modules_test.cpp
    "${RNQJS_GENERATED_REGISTRY}")
  target_link_libraries(quickjs_generated_modules_tests PRIVATE
    quickjs_jsi
    react-native-quickjs-intl
    react-native-quickjs-text-encoding
    GTest::gtest GTest::gtest_main)
  set_target_properties(quickjs_generated_modules_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  add_test(NAME generated_modules COMMAND quickjs_generated_modules_tests)
endif()
