# react-native-quickjs-text-encoding
#
# Shared by the Android and Apple builds. Compiles from source against the
# QuickJS engine the app already builds — nothing here is ever prebuilt,
# because the engine is compiled per app and a prebuilt module would be linked
# against a different engine than the one it runs in.

cmake_minimum_required(VERSION 3.20)

set(RNQJS_MODULE_NAME react-native-quickjs-text-encoding)

# Locate react-native-quickjs. Overridable so a monorepo can point at a
# workspace copy rather than a hoisted one.
if(NOT DEFINED REACT_NATIVE_QUICKJS_DIR)
  set(REACT_NATIVE_QUICKJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs"
      CACHE PATH "Path to the react-native-quickjs package")
endif()

if(NOT EXISTS "${REACT_NATIVE_QUICKJS_DIR}/src/module/QuickJSModule.h")
  message(FATAL_ERROR
    "Could not find react-native-quickjs at ${REACT_NATIVE_QUICKJS_DIR}. "
    "Pass -DREACT_NATIVE_QUICKJS_DIR=/path/to/react-native-quickjs")
endif()

set(REACT_NATIVE_QUICKJS_DIR "${REACT_NATIVE_QUICKJS_DIR}" PARENT_SCOPE)

add_library(${RNQJS_MODULE_NAME} STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/TextEncodingModule.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp/TextEncodingPlatform.cpp
)

target_include_directories(${RNQJS_MODULE_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp
  ${REACT_NATIVE_QUICKJS_DIR}/src/bytecode
  ${REACT_NATIVE_QUICKJS_DIR}/src/module
  ${REACT_NATIVE_QUICKJS_DIR}/src/runtime
  ${REACT_NATIVE_QUICKJS_DIR}/engine/quickjs-rel
)

target_compile_features(${RNQJS_MODULE_NAME} PUBLIC cxx_std_20)
set_target_properties(${RNQJS_MODULE_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# TextEncodingModule.cpp includes <jsi/jsi.h> unconditionally; which target
# provides it depends on the build (ReactAndroid::jsi on Android, jsi on host).
if(TARGET ReactAndroid::jsi)
  target_link_libraries(${RNQJS_MODULE_NAME} PUBLIC ReactAndroid::jsi)
elseif(TARGET jsi)
  target_link_libraries(${RNQJS_MODULE_NAME} PUBLIC jsi)
else()
  message(FATAL_ERROR
    "react-native-quickjs-text-encoding needs JSI. Build it where "
    "ReactAndroid::jsi (Android) or jsi (host) is defined.")
endif()

# Host tests. Only when this is built as part of the repository, where the
# engine and googletest are already configured.
if(TARGET quickjs_jsi AND TARGET GTest::gtest)
  target_link_libraries(${RNQJS_MODULE_NAME} PUBLIC quickjs_jsi)

  add_executable(text_encoding_tests test/text_encoding_test.cpp)
  target_link_libraries(text_encoding_tests
    PRIVATE ${RNQJS_MODULE_NAME} GTest::gtest GTest::gtest_main)
  set_target_properties(text_encoding_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  add_test(NAME text_encoding COMMAND text_encoding_tests)

  # The numbers mean nothing unless the engine it links was optimized, so the
  # benchmark says so and reports nothing rather than printing a Debug figure
  # someone might quote.
  add_executable(text_encoding_bench bench/text_encoding_bench.cpp)
  target_link_libraries(text_encoding_bench PRIVATE ${RNQJS_MODULE_NAME})
  target_compile_definitions(text_encoding_bench PRIVATE
    QJS_BENCH_OPTIMIZED=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>,1,0>)
  set_target_properties(text_encoding_bench PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
endif()
