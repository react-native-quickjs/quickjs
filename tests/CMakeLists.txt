# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_executable(quickjs_jsi_bench bench/jsi_bench.cpp)
target_link_libraries(quickjs_jsi_bench PRIVATE quickjs_jsi)

# The benchmark reports nothing unless the engine it links was optimized.
target_compile_definitions(quickjs_jsi_bench PRIVATE
  QJS_BENCH_OPTIMIZED=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>,1,0>)

add_test(NAME jsi_bench COMMAND quickjs_jsi_bench)
