# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# The protocol layer needs <quickjs.h> and nothing else, which is what lets its
# test run anywhere.

add_library(quickjs_cdp STATIC quickjs-cdp.c)
target_include_directories(quickjs_cdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(quickjs_cdp PUBLIC quickjs)
set_target_properties(quickjs_cdp PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

if(TARGET GTest::gtest)
  add_executable(quickjs_cdp_tests test/cdp_test.cpp)
  target_link_libraries(quickjs_cdp_tests
    PRIVATE quickjs_cdp GTest::gtest GTest::gtest_main)
  add_test(NAME cdp COMMAND quickjs_cdp_tests)
endif()
