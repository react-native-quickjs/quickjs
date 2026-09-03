# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# TextEncoder and TextDecoder. See README.md.
#
# The implementation itself is compiled into quickjs_jsi rather than into a
# library of its own: it installs from the runtime factory, and a static
# library the linker is free to drop is exactly how a module ends up compiled,
# linked and silently absent. This file only adds the tests.

if(TARGET GTest::gtest)
  add_executable(text_encoding_tests test/text_encoding_test.cpp)
  target_link_libraries(text_encoding_tests
    PRIVATE quickjs_jsi GTest::gtest GTest::gtest_main)
  set_target_properties(text_encoding_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  add_test(NAME text_encoding COMMAND text_encoding_tests)
endif()
