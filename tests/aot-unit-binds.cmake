# The AOT product path must BIND the unit.
#
# The engine refers to `qjs_aot_table` weakly, so that a build with no unit
# still links. A weak undefined reference does not pull its member out of a
# static archive, so the unit is compiled, archived, and then dropped: the
# binary links, exits 0, and silently runs the interpreter. Naming the symbol
# on the link line is what forces the member in.
#
# The test runs the linked binary on a kernel that reaches compiled code with
# the unit statistics on, and requires the unit to be bound AND entered: a
# dropped unit leaves unit=0 / enter=0 while still exiting 0, which is exactly
# why this asserts on the statistics and not on the exit code.
if(NOT EXISTS "${BIN}")
  message(FATAL_ERROR "aot_unit_binds: no binary at ${BIN}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env QJS_AOT_STATS=1 "${BIN}"
          "${ROOT}/tools/aot/kernels2/02-loops.js"
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "aot_unit_binds: the program failed (${rc})\n${out}\n${err}")
endif()

if(NOT err MATCHES "unit=1")
  message(FATAL_ERROR
    "aot_unit_binds: the unit is not bound, so the program ran in the interpreter.\n"
    "  A weak reference does not pull the unit out of a static archive: the link\n"
    "  must name the symbol (-u qjs_aot_table, or the Apple/MSVC form).\n"
    "  stderr was:\n${err}")
endif()
if(err MATCHES "enter=0")
  message(FATAL_ERROR
    "aot_unit_binds: the unit is bound but never entered.\n  stderr was:\n${err}")
endif()
message(STATUS "aot_unit_binds: unit bound and entered")
