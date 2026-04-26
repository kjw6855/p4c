if(ENABLE_TESTING)
  # Include the test subdirectory.
  message("-- Adding symbex bmv2 test suite")
  include(test/P4Tests.cmake)
endif()

# Source files for symbex.
set(SYMBEX_SOURCES
  ${SYMBEX_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/common.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/protobuf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/protobuf_ir.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/metadata.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/ptf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend/stf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cmd_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cmd_visitor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compiler_result.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/concolic.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/constants.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/expr_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/expr_visitor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/map_direct_externs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/p4_asserts_parser.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/p4_refers_to_parser.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/p4runtime_translation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/program_info.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/table_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/table_visitor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/target.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_spec.cpp
  PARENT_SCOPE
)

set(SYMBEX_GTEST_SOURCES
  ${SYMBEX_GTEST_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/test/gtest_utils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/symbex_api/api_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/symbex_api/benchmark.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/symbex_api/control_plane_filter_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/symbex_api/output_option_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/test_backend/ptf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/test_backend/stf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/small-step/binary.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/small-step/unary.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/small-step/p4_asserts_parser_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/small-step/value.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/small-step/reachability.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/z3-solver/saturation_arithm.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/z3-solver/expressions.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test/z3-solver/asrt_model.cpp
  PARENT_SCOPE
)


# Link the run-bmv2-test binary
execute_process(COMMAND ln -sfn ${P4C_SOURCE_DIR}/backends/bmv2/run-bmv2-test.py ${CMAKE_BINARY_DIR}/run-bmv2-test.py)

set(
  SYMBEX_LIBS ${SYMBEX_LIBS}
  PARENT_SCOPE
)

