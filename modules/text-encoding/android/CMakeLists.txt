cmake_minimum_required(VERSION 3.20)
project(text_encoding)

# The shared, platform-independent C++.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." text_encoding_shared)

# The Android platform layer: JNI glue that lets the shared C++ reach Android
# APIs (android.icu, KeyStore, ContentProvider, anything on the Java side).
add_library(text_encoding_platform SHARED
  src/main/cpp/TextEncodingPlatform.cpp
)

target_include_directories(text_encoding_platform PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp
  ${REACT_NATIVE_QUICKJS_DIR}/src/bytecode
  ${REACT_NATIVE_QUICKJS_DIR}/src/module
  ${REACT_NATIVE_QUICKJS_DIR}/src/runtime
  ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel
)

target_link_libraries(text_encoding_platform
  ${RNQJS_MODULE_NAME}
  log
)
