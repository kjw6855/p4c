# CMake file for Symbex.

include(common)
include(gRPC)
p4c_obtain_grpc()

project(symbex)

# Declare common Symbex variables.
set(SYMBEX_DIR ${P4C_BINARY_DIR}/symbex)
set(SYMBEX_DRIVER "${CMAKE_CURRENT_BINARY_DIR}/p4symbex")

get_filename_component(p4symbex_proto_path "./targets/bmv2/proto" ABSOLUTE)
set(p4symbex_proto "${p4symbex_proto_path}/p4symbex.proto")
set(p4runtime_proto_path "${P4RUNTIME_STD_DIR}")
set(p4runtime_cp_path "${P4C_SOURCE_DIR}/control-plane")

set(p4symbex_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/p4symbex.grpc.pb.cc")
set(p4symbex_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/p4symbex.grpc.pb.h")
set(p4symbex_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/p4symbex.pb.cc")
set(p4symbex_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/p4symbex.pb.h")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/p4runtime")
add_custom_target(link_target ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${P4C_BINARY_DIR}/control-plane"
        "${CMAKE_CURRENT_BINARY_DIR}/p4runtime/proto")

add_custom_command(
      OUTPUT "${p4symbex_proto_srcs}" "${p4symbex_proto_hdrs}" "${p4symbex_grpc_srcs}" "${p4symbex_grpc_hdrs}"
      COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${p4symbex_proto_path}"
        -I "${P4RUNTIME_STD_DIR}"
        -I "${p4runtime_cp_path}"
        -I "${protobuf_SOURCE_DIR}/src"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${p4symbex_proto}"
      DEPENDS
        ${p4symbex_proto}
        ${P4RUNTIME_STD_DIR}/p4/v1/p4runtime.proto
      COMMENT "Generating proto files at ${CMAKE_CURRENT_BINARY_DIR} by using ${Protobuf_PROTOC_EXECUTABLE}, depending ${p4runtime_SOURCE_DIR}, and importing ${p4symbex_proto_path}, ${P4RUNTIME_STD_DIR}, and ${p4runtime_cp_path}")

set(
  SYMBEX_SOURCES
  async_server.cpp
  options.cpp
  symbex.cpp

  core/compiler_result.cpp
  core/program_info.cpp
  core/small_step/abstract_stepper.cpp
  core/small_step/cmd_stepper.cpp
  core/small_step/expr_stepper.cpp
  core/small_step/extern_stepper.cpp
  core/small_step/table_stepper.cpp
  core/small_step/small_step.cpp
  core/small_visit/abstract_visitor.cpp
  core/small_visit/cmd_visitor.cpp
  core/small_visit/expr_visitor.cpp
  core/small_visit/extern_visitor.cpp
  core/small_visit/table_visitor.cpp
  core/small_visit/small_visit.cpp
  core/symbolic_executor/cp_annotation.cpp
  core/symbolic_executor/depth_first.cpp
  core/symbolic_executor/selected_branches.cpp
  core/symbolic_executor/sink_divergence.cpp
  core/symbolic_executor/random_backtrack.cpp
  core/symbolic_executor/greedy_node_cov.cpp
  core/symbolic_executor/state_dependency_track.cpp
  core/symbolic_executor/symbolic_executor.cpp
  core/concolic_executor/concolic_executor.cpp
  core/target.cpp

  lib/collect_coverable_nodes.cpp
  lib/concolic.cpp
  lib/visit_concolic.cpp
  lib/continuation.cpp
  lib/execution_state.cpp
  lib/final_state.cpp
  lib/final_visit_state.cpp
  lib/logging.cpp
  lib/packet_vars.cpp
  lib/table_collector.cpp
  lib/test_backend.cpp
  lib/test_framework.cpp
  lib/test_spec.cpp

  lib/graphs/controls.cpp
  lib/graphs/graphs.cpp
  lib/graphs/parsers.cpp

  ${p4symbex_proto_srcs}
  ${p4symbex_proto_hdrs}
  ${p4symbex_grpc_srcs}
  ${p4symbex_grpc_hdrs}
)

# GTest source files for symbex.
set(
  SYMBEX_GTEST_SOURCES
  #  # XXX These should be in a library.
  ${P4C_SOURCE_DIR}/test/gtest/helpers.cpp
  ${P4C_SOURCE_DIR}/test/gtest/gtestp4c.cpp

  test/gtest_utils.cpp
  test/lib/format_int.cpp
  test/lib/p4info_api.cpp
  test/lib/taint.cpp
  test/small-step/util.cpp
  test/z3-solver/constraints.cpp
)

# Inja is needed to produce test templates.
set(INJA_BUILD_TESTS OFF CACHE BOOL "Build unit tests when BUILD_TESTING is enabled.")
set(BUILD_BENCHMARK OFF CACHE BOOL "Build benchmark.")
FetchContent_Declare(
  inja
  GIT_REPOSITORY https://github.com/pantor/inja.git
  GIT_TAG        3741c73ba78babd2ed88f2acf2fcd6dafdb878e8
  GIT_PROGRESS TRUE
)
fetchcontent_makeavailable_but_exclude_install(inja)

# Symbex libraries and includes. Defined here such that targets can extend them.
set(
  SYMBEX_LIBS
  PRIVATE p4tools-common
  PRIVATE p4tools-control-plane
  PRIVATE p4csd-lib
  # Make sure the symbex library and anything that links to it see both the Inja includes.
  PUBLIC inja
  PUBLIC gRPC::grpc++
  PUBLIC gRPC::gpr
  PUBLIC ${PROTOBUF_LIBRARY}
)
set(SYMBEX_INCLUDES)

# Only enable Tofino tests if the Tofino backend is enabled.
# FIXME: Can we make this part of the extension?
if (NOT ENABLE_TOFINO)
  set (ENABLE_TOOLS_TARGET_TOFINO OFF)
endif()

file(GLOB symbex_targets RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/targets ${CMAKE_CURRENT_SOURCE_DIR}/targets/*)
foreach(ext ${symbex_targets})
  set(symbex_targets_dir ${CMAKE_CURRENT_SOURCE_DIR}/targets/${ext}/)
  if(EXISTS ${symbex_targets_dir}/CMakeLists.txt AND IS_DIRECTORY ${symbex_targets_dir})
    # Generate an option that makes it possible to disable this extension.
    string(MAKE_C_IDENTIFIER ${ext} EXT_AS_IDENTIFIER)
    string(TOUPPER ${EXT_AS_IDENTIFIER} EXT_AS_OPTION_NAME)
    string(CONCAT ENABLE_EXT_OPTION "ENABLE_TOOLS_TARGET_" ${EXT_AS_OPTION_NAME})
    string(CONCAT EXT_HELP_TEXT "Build the " ${ext} " target")
    option(${ENABLE_EXT_OPTION} ${EXT_HELP_TEXT} ON)
    if(${ENABLE_EXT_OPTION})
      message("-- Enabling target ${ext}")
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/targets/${ext})
      set(include_statements_var "${include_statements_var}#include \"backends/p4tools/modules/symbex/targets/${ext}/register.h\"\n")
      set(symbex_targets_var "${symbex_targets_var}    ${ext}RegisterSymbexTarget();\n")
    endif()
  endif()
endforeach(ext)

# Propagate def files set by target extensions upwards.
set(IR_DEF_FILES ${IR_DEF_FILES} PARENT_SCOPE)

# Fill the template
configure_file(register.h.in register.h)

# The library (named symbex-lib to avoid collision with the symbex executable target).
add_library(symbex-lib STATIC ${SYMBEX_SOURCES})
target_link_libraries(symbex-lib
  ${SYMBEX_LIBS}
  # For Abseil includes.
  PRIVATE frontend
  # For Protobuf includes.
  PRIVATE controlplane
)
if (SYMBEX_INCLUDES)
  target_include_directories(symbex-lib ${SYMBEX_INCLUDES})
endif()

target_include_directories(symbex-lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")


# The executable.
add_p4tools_executable(p4symbex main.cpp)
target_link_libraries(
  p4symbex
  PRIVATE symbex-lib
  ${SYMBEX_LIBS}
  PRIVATE ${P4C_LIBRARIES}
  PRIVATE ${P4C_LIB_DEPS}
)

add_custom_target(
  linkp4symbex
  # Create binary symlink in build root.
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_BINARY_DIR}/p4symbex ${P4C_BINARY_DIR}/p4symbex
  # Copy p4include files into the build tree.
  COMMAND ${CMAKE_COMMAND} -E make_directory ${P4C_BINARY_DIR}/p4include &&
          ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/p4include/*.p4 ${P4C_BINARY_DIR}/p4include
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${P4C_BINARY_DIR}/p4include ${CMAKE_CURRENT_BINARY_DIR}/p4include
  DEPENDS update_includes
  COMMENT "Setting Symbex include symlinks."
)

add_dependencies(p4symbex linkp4symbex)

if(ENABLE_GTESTS)
  add_executable(p4symbex-gtest ${SYMBEX_GTEST_SOURCES})
  target_link_libraries(
    p4symbex-gtest
    PRIVATE symbex-lib
    PRIVATE gtest
    PRIVATE ${SYMBEX_LIBS}
    PRIVATE ${P4C_LIBRARIES}
    PRIVATE ${P4C_LIB_DEPS}
  )

  if(ENABLE_TESTING)
    add_test(NAME p4symbex-gtest COMMAND p4symbex-gtest)
    set_tests_properties(p4symbex-gtest PROPERTIES LABELS "gtest-symbex")
  endif()

endif()
