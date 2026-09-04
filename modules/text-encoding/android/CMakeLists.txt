# Folded into the engine's single .so (see the engine's android/CMakeLists.txt):
# the engine add_subdirectory's this directory and whole-archives the static
# libraries declared here into libquickjsinstancejni.so.

cmake_minimum_required(VERSION 3.20)
project(text_encoding)

set(RNQJS_MODULE_NAME react-native-quickjs-text-encoding)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." text_encoding_shared)

# Platform JNI. RNQJS_QUICKJS_FOLDED makes TextEncodingPlatform.cpp omit its
# JNI_OnLoad (one per .so) and capture the JavaVM from its attach call.
add_library(${RNQJS_MODULE_NAME}-platform STATIC
  src/main/cpp/TextEncodingPlatform.cpp
)

target_include_directories(${RNQJS_MODULE_NAME}-platform PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp
)

target_compile_features(${RNQJS_MODULE_NAME}-platform PUBLIC cxx_std_20)
set_target_properties(${RNQJS_MODULE_NAME}-platform PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

# Whole-archived into libquickjsinstancejni.so by the engine.
set(RNQJS_QUICKJS_MODULE_TARGETS
  "${RNQJS_MODULE_NAME};${RNQJS_MODULE_NAME}-platform"
  PARENT_SCOPE)
