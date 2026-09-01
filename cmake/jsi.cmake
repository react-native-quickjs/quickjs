# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Defines the `jsi` static library.
#
# It compiles React Native's own copy of JSI, not a vendored one. jsi::Runtime
# is abstract, so building against different headers than the app links is a
# vtable mismatch: it compiles, links, and crashes at the first virtual call.

set(REACT_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../node_modules/react-native"
    CACHE PATH "Path to the react-native package")
set(JSI_DIR "${REACT_NATIVE_DIR}/ReactCommon/jsi")

if(NOT EXISTS "${JSI_DIR}/jsi/jsi.cpp")
  message(FATAL_ERROR
    "Could not find JSI at ${JSI_DIR}.\n"
    "Run `npm install` first, or pass -DREACT_NATIVE_DIR=/path/to/react-native")
endif()

add_library(jsi STATIC "${JSI_DIR}/jsi/jsi.cpp")
target_include_directories(jsi PUBLIC "${JSI_DIR}")
set_target_properties(jsi PROPERTIES POSITION_INDEPENDENT_CODE ON)
