# react-native-quickjs-intl, folded into the engine's single .so.
#
# The engine's android/CMakeLists.txt add_subdirectory's this directory when
# the package is installed, and whole-archives the two static libraries it
# declares. A module never builds its own .so: it would load before the engine
# (the app instantiates packages before the runtime factory) and fail to
# resolve the engine's JS_* API.

cmake_minimum_required(VERSION 3.20)
project(intl)

# The shared, platform-independent core (cpp/*, including the JSI edge and the
# build-time compilation of js/intl.js to bytecode). See ../CMakeLists.txt.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." intl_shared)

# The Android platform layer: hand-written JNI glue that lets the shared C++
# reach android.icu. No fbjni -- see src/main/cpp/RnqjsJni.h for why.
#
# The engine compiles this whole directory with RNQJS_QUICKJS_FOLDED, which
# makes IntlPlatform.cpp omit its JNI_OnLoad (a .so has one) and capture the
# JavaVM from the JNIEnv its attach call arrives with instead.
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

# What the engine whole-archives into libquickjsinstancejni.so.
set(RNQJS_QUICKJS_MODULE_TARGETS
  "${RNQJS_MODULE_NAME};${RNQJS_MODULE_NAME}-platform"
  PARENT_SCOPE)
