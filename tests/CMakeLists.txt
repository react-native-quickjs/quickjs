# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# googletest is fetched rather than vendored or required from the system: the
# suites below are the gate on every stage, and a contributor should not have to
# install anything to run them. Pinned to a tag so a CI run and a local run test
# the same thing.
include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  # v1.18.0 and not older: React Native's inspector test suite, which
  # modules/cdp runs as its compatibility gate, uses gmock's
  # WithoutMatchers::Get(). That accessor was a free function until this tag.
  GIT_TAG v1.18.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

function(qjs_add_test_suite name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE quickjs_jsi GTest::gtest GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# No gtest here on purpose -- see the file header.
add_executable(quickjs_jsi_tests smoketest.cpp)
target_link_libraries(quickjs_jsi_tests PRIVATE quickjs_jsi)
add_test(NAME quickjs_jsi_tests COMMAND quickjs_jsi_tests)

# Hermes' own JSI conformance suite, parameterised over a RuntimeFactory, so
# supplying ours is the whole integration. testlib.h is included as
# <jsi/test/testlib.h>: only third_party/ goes on the include path, so
# <jsi/jsi.h> still resolves to React Native's JSI. See third_party/README.md.
qjs_add_test_suite(quickjs_jsi_conformance
  conformance.cpp
  third_party/jsi/test/testlib.cpp
)
target_include_directories(quickjs_jsi_conformance PRIVATE third_party)

# Shells out to the compiler built from the same engine, so the tests exercise
# the real container rather than a hand-written fixture.
qjs_add_test_suite(quickjs_bytecode_tests bytecode_test.cpp)
target_compile_definitions(quickjs_bytecode_tests
  PRIVATE QJSC_PATH="$<TARGET_FILE:qjsc>")
add_dependencies(quickjs_bytecode_tests qjsc)

# Drives the C API on JSRuntime/JSContext directly rather than through JSI,
# because that is the surface a debugger bridge sits on.
qjs_add_test_suite(quickjs_debugger_tests debugger_test.cpp)

qjs_add_test_suite(quickjs_module_tests modules_test.cpp)

qjs_add_test_suite(quickjs_runtime_tests runtime_test.cpp)

# --- differential corpus ---------------------------------------------------
#
# Every corpus file is run by this engine and by node, and the two outputs are
# compared byte for byte. An expectation written by the same person who wrote
# the code encodes the same misunderstanding; node is an independent
# implementation of the same specification, so a diff is evidence.
add_executable(qjs-run differential/qjs-run.c)
target_link_libraries(qjs-run PRIVATE quickjs)

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  # Twice, because they are not the same path: React Native ships bytecode, and
  # anything the compiler derives from source that the serializer drops is
  # present in one run and absent in the other.
  add_test(NAME differential
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential/run.mjs
            --qjs $<TARGET_FILE:qjs-run>)
  add_test(NAME differential_bytecode
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential/run.mjs
            --qjs $<TARGET_FILE:qjs-run> --qjsc $<TARGET_FILE:qjsc>
            --via-bytecode)
else()
  message(STATUS "rnqjs: node not found, differential corpus will not run")
endif()

add_executable(quickjs_jsi_bench bench/jsi_bench.cpp)
target_link_libraries(quickjs_jsi_bench PRIVATE quickjs_jsi)

# The benchmark reports nothing unless the engine it links was optimized.
target_compile_definitions(quickjs_jsi_bench PRIVATE
  QJS_BENCH_OPTIMIZED=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>,1,0>)

add_test(NAME jsi_bench COMMAND quickjs_jsi_bench)
