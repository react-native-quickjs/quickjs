# Folded into the engine's single .so (see the engine's android/CMakeLists.txt):
# the engine add_subdirectory's this directory and whole-archives the static
# libraries declared here into libquickjsinstancejni.so.

cmake_minimum_required(VERSION 3.20)
project(intl)

# The shared, platform-independent core (cpp/*, including the JSI edge and the
# build-time compilation of js/intl.js to bytecode). See ../CMakeLists.txt.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." intl_shared)

# Platform JNI glue to android.icu (no fbjni -- see cpp/RnqjsJni.h). Under
# RNQJS_QUICKJS_FOLDED, IntlPlatform.cpp omits its JNI_OnLoad (one per .so) and
# captures the JavaVM from its attach call instead.
add_library(${RNQJS_MODULE_NAME}-platform STATIC
  src/main/cpp/IntlPlatform.cpp
  src/main/cpp/RnqjsJni.cpp
)

target_include_directories(${RNQJS_MODULE_NAME}-platform PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp
)

target_compile_features(${RNQJS_MODULE_NAME}-platform PUBLIC cxx_std_20)
set_target_properties(${RNQJS_MODULE_NAME}-platform PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

# Whole-archived into libquickjsinstancejni.so by the engine.
set(RNQJS_QUICKJS_MODULE_TARGETS
  "${RNQJS_MODULE_NAME};${RNQJS_MODULE_NAME}-platform"
  PARENT_SCOPE)
