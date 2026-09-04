cmake_minimum_required(VERSION 3.20)
project(intl)

# The shared, platform-independent C++ — including the build-time compilation
# of js/intl.js to bytecode, which the parent CMakeLists.txt owns.
#
# An Android build cross-compiles, so the qjsc-ng it would build is for the
# device and cannot run on the build host. Pass -DRNQJS_INTL_QJSC=<host qjsc-ng>
# through gradle's externalNativeBuild arguments; without it the parent warns
# and embeds the source instead, which still works and still stays lazy but
# pays a parse on first use of Intl.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." intl_shared)

# The Android platform layer: hand-written JNI glue that lets the shared C++
# reach android.icu. No fbjni — see src/main/cpp/RnqjsJni.h for why.
add_library(intl_platform SHARED
  src/main/cpp/IntlPlatform.cpp
  src/main/cpp/RnqjsJni.cpp
)

# The JNI glue talks to android.icu and to the shared C++ seam in ../cpp. It
# never includes a quickjs header, so it needs no engine include path of its
# own; everything else comes transitively from ${RNQJS_MODULE_NAME}.
target_include_directories(intl_platform PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp
)

target_link_libraries(intl_platform
  ${RNQJS_MODULE_NAME}
  log
)
