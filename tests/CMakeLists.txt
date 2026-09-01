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
  GIT_TAG v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Every gtest suite here wants the same three things.
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

add_executable(quickjs_jsi_bench bench/jsi_bench.cpp)
target_link_libraries(quickjs_jsi_bench PRIVATE quickjs_jsi)

# The benchmark reports nothing unless the engine it links was optimized.
target_compile_definitions(quickjs_jsi_bench PRIVATE
  QJS_BENCH_OPTIMIZED=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>,1,0>)

add_test(NAME jsi_bench COMMAND quickjs_jsi_bench)
