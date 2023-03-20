# CMake file for Barefoot p4fuzzer.
cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

include(common)

project(p4fuzzer)

# Add p4fuzzer.proto grpc service
get_filename_component(p4fuzzer_proto_path "./targets/bmv2/proto" ABSOLUTE)
set(p4fuzzer_proto "${p4fuzzer_proto_path}/p4fuzzer.proto")
set(p4testgen_proto "${p4fuzzer_proto_path}/p4testgen.proto")
set(p4runtime_proto_path "${P4C_SOURCE_DIR}/control-plane/p4runtime/proto")
set(p4runtime_cp_path "${P4C_SOURCE_DIR}/control-plane")

# Generated sources from p4fuzzer.proto
set(p4fuzzer_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/p4fuzzer.pb.cc")
set(p4fuzzer_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/p4fuzzer.pb.h")
set(p4fuzzer_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/p4fuzzer.grpc.pb.cc")
set(p4fuzzer_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/p4fuzzer.grpc.pb.h")

set(p4testgen_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/p4testgen.pb.cc")
set(p4testgen_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/p4testgen.pb.h")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/p4runtime")
add_custom_target(link_target ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${P4C_BINARY_DIR}/control-plane"
        "${CMAKE_CURRENT_BINARY_DIR}/p4runtime/proto")

add_custom_command(
      OUTPUT "${p4testgen_proto_srcs}" "${p4testgen_proto_hdrs}"
      COMMAND "${PROTOBUF_PROTOC_EXECUTABLE}"
      ARGS --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${p4fuzzer_proto_path}"
        -I "${p4runtime_cp_path}"
        -I "${p4runtime_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${p4testgen_proto}"
      DEPENDS "${p4testgen_proto}")

add_custom_command(
      OUTPUT "${p4fuzzer_proto_srcs}" "${p4fuzzer_proto_hdrs}" "${p4fuzzer_grpc_srcs}" "${p4fuzzer_grpc_hdrs}"
      COMMAND "${PROTOBUF_PROTOC_EXECUTABLE}"
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${p4fuzzer_proto_path}"
        -I "${p4runtime_cp_path}"
        -I "${p4runtime_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${p4fuzzer_proto}"
      DEPENDS "${p4fuzzer_proto}")

# Source files for p4fuzzer.
set(
  FUZZER_SOURCES
  options.cpp
  fuzzer.cpp

  core/arch_spec.cpp
  core/externs.cpp
  core/program_info.cpp
  core/small_step/abstract_stepper.cpp
  core/small_step/cmd_stepper.cpp
  core/small_step/expr_stepper.cpp
  core/small_step/extern_stepper.cpp
  core/small_step/table_stepper.cpp
  core/small_step/small_step.cpp
  core/exploration_strategy/incremental_stack.cpp
  core/exploration_strategy/selected_branches.cpp
  core/exploration_strategy/random_access_stack.cpp
  core/exploration_strategy/linear_enumeration.cpp
  core/exploration_strategy/rnd_access_max_coverage.cpp
  core/exploration_strategy/inc_max_coverage_stack.cpp
  core/exploration_strategy/exploration_strategy.cpp
  core/target.cpp

  lib/concolic.cpp
  lib/continuation.cpp
  lib/execution_state.cpp
  lib/final_state.cpp
  lib/gen_eq.cpp
  lib/logging.cpp
  lib/namespace_context.cpp
  lib/test_backend.cpp
  lib/test_spec.cpp
  lib/tf.cpp

  ${p4testgen_proto_srcs}
  ${p4testgen_proto_hdrs}

  ${p4fuzzer_proto_srcs}
  ${p4fuzzer_proto_hdrs}
  ${p4fuzzer_grpc_srcs}
  ${p4fuzzer_grpc_hdrs}
)

# GTest source files for p4fuzzer.
set(
  FUZZER_GTEST_SOURCES
  #  # XXX These should be in a library.
  ${P4C_SOURCE_DIR}/test/gtest/helpers.cpp
  ${P4C_SOURCE_DIR}/test/gtest/gtestp4c.cpp

  test/gtest_utils.cpp
  test/lib/format_int.cpp
  test/lib/taint.cpp
  test/small-step/binary.cpp
  test/small-step/reachability.cpp
  test/small-step/unary.cpp
  test/small-step/util.cpp
  test/small-step/value.cpp
  test/small-step/p4_asserts_parser_test.cpp
  test/transformations/saturation_arithm.cpp
  test/z3-solver/asrt_model.cpp
  test/z3-solver/expressions.cpp
)

# Fuzzer libraries.
set(
  FUZZER_LIBS
  p4tools-common
  inja
  grpc++
  ${PROTOBUF_LIBRARY}
)

file(GLOB fuzzer_targets RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/targets ${CMAKE_CURRENT_SOURCE_DIR}/targets/*)
foreach(ext ${fuzzer_targets})
  set(fuzzer_targets_dir ${CMAKE_CURRENT_SOURCE_DIR}/targets/${ext}/)
  if(EXISTS ${fuzzer_targets_dir}/CMakeLists.txt AND IS_DIRECTORY ${fuzzer_targets_dir})
    # Generate an option that makes it possible to disable this extension.
    string(MAKE_C_IDENTIFIER ${ext} EXT_AS_IDENTIFIER)
    string(TOUPPER ${EXT_AS_IDENTIFIER} EXT_AS_OPTION_NAME)
    string(CONCAT ENABLE_EXT_OPTION "ENABLE_FUZZER_TARGET_" ${EXT_AS_OPTION_NAME})
    string(CONCAT EXT_HELP_TEXT "Build the " ${ext} " target")
    option(${ENABLE_EXT_OPTION} ${EXT_HELP_TEXT} ON)
    if(${ENABLE_EXT_OPTION})
      message("-- Enabling target ${ext}")
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/targets/${ext})
      set(include_statements_var "${include_statements_var}#include \"backends/p4tools/modules/fuzzer/targets/${ext}/register.h\"\n")
      set(compiler_targets_var "${compiler_targets_var}    ${ext}_registerCompilerTarget();\n")
      set(fuzzer_targets_var "${fuzzer_targets_var}    ${ext}_registerFuzzerTarget();\n")
    endif()
  endif()
endforeach(ext)

# Propagate def files set by target extensions upwards.
set(IR_DEF_FILES ${IR_DEF_FILES} PARENT_SCOPE)

# Convert the list of files into #includes
foreach(include_file ${include_files})
endforeach()

# Fill the template
configure_file(register.h.in register.h)

add_p4tools_library(fuzzer ${FUZZER_SOURCES})

target_link_libraries(
  fuzzer
  ${FUZZER_LIBS}
)

add_p4tools_executable(p4fuzzer main.cpp)

target_link_libraries(
  p4fuzzer
  fuzzer
  ${FUZZER_LIBS}
)

add_custom_target(
  linkp4fuzzer
  # Add some convenience links for invoking p4fuzzer.
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_BINARY_DIR}/p4fuzzer ${CMAKE_BINARY_DIR}/p4fuzzer
)

add_dependencies(p4fuzzer linkp4fuzzer)

if(ENABLE_GTESTS)
  add_executable(fuzzer-gtest ${FUZZER_GTEST_SOURCES})
  target_include_directories(
    fuzzer-gtest
    PRIVATE ${P4C_SOURCE_DIR}/test/frameworks/gtest/googlemock/include
    PRIVATE ${P4C_SOURCE_DIR}/test/frameworks/gtest/googletest/include
  )
  target_link_libraries(
    fuzzer-gtest
    PRIVATE fuzzer
    PRIVATE gtest
  )

  add_custom_target(
    fuzzer-linkgtest
    # Link P4 include files in a more convenient location.
    COMMAND
    for incl in p4include p4_14include \; do
    ${CMAKE_COMMAND} -E create_symlink
    ${P4C_BINARY_DIR}/\$$incl ${CMAKE_CURRENT_BINARY_DIR}/\$$incl \;
    done
  )
  add_dependencies(fuzzer-gtest fuzzer-linkgtest)

  if(ENABLE_TESTING)
    add_definitions("-DGTEST_HAS_PTHREAD=0")
    add_test(NAME fuzzer-gtest COMMAND fuzzer-gtest)
    set_tests_properties(fuzzer-gtest PROPERTIES LABELS "gtest-fuzzer")
  endif()

  if(ENABLE_IWYU)
    set_property(TARGET fuzzer-gtest PROPERTY CXX_INCLUDE_WHAT_YOU_USE ${iwyu_path})
  endif()
endif()

if(ENABLE_IWYU)
  set_property(TARGET fuzzer PROPERTY CXX_INCLUDE_WHAT_YOU_USE ${iwyu_path})
endif()
