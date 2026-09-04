# react-native-quickjs-text-encoding, folded into the engine's single .so.
#
# See modules/intl/android/CMakeLists.txt for the contract: the engine's
# android/CMakeLists.txt add_subdirectory's this directory and whole-archives
# the static libraries declared here into libquickjsinstancejni.so.

cmake_minimum_required(VERSION 3.20)
project(text_encoding)

# The shared, platform-independent core (cpp/*). See ../CMakeLists.txt.
set(RNQJS_MODULE_NAME react-native-quickjs-text-encoding)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." text_encoding_shared)

# The Android platform layer: JNI glue that lets the shared C++ reach Android
# APIs. The engine compiles this directory with RNQJS_QUICKJS_FOLDED, which
# makes TextEncodingPlatform.cpp omit its JNI_OnLoad (a .so has one) and
# capture the JavaVM from the JNIEnv its attach call arrives with instead.
add_library(${RNQJS_MODULE_NAME}-platform STATIC
  src/main/cpp/TextEncodingPlatform.cpp
)

target_include_directories(${RNQJS_MODULE_NAME}-platform PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp
)

target_compile_features(${RNQJS_MODULE_NAME}-platform PUBLIC cxx_std_20)
set_target_properties(${RNQJS_MODULE_NAME}-platform PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

# What the engine whole-archives into libquickjsinstancejni.so.
set(RNQJS_QUICKJS_MODULE_TARGETS
  "${RNQJS_MODULE_NAME};${RNQJS_MODULE_NAME}-platform"
  PARENT_SCOPE)
