# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# The host build, used for development and the test suites. Android builds
# through platform/android and Apple through the podspec; all three reuse
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
  src/runtime/QuickJSRuntime.cpp
)
target_include_directories(quickjs_jsi PUBLIC src/bytecode src/runtime)
target_link_libraries(quickjs_jsi PUBLIC jsi quickjs)
set_target_properties(quickjs_jsi PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Built from the engine in this tree, not shipped prebuilt: bytecode is only
# loadable by the exact engine build that produced it.
add_executable(qjsc-ng tools/bytecode/qjsc-ng.c)
target_link_libraries(qjsc-ng PRIVATE quickjs)

add_subdirectory(tests)
