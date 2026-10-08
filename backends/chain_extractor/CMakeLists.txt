configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.h.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

# Analysis sources: no main(), no options — usable as a library by other tools.
set (CHAIN_EXTRACTOR_LIB_SRCS
  analysis.cpp
  dependency_graph.cpp
  chain_pass.cpp
  chain_cache.cpp
  parser_deps.cpp
)

set (CHAIN_EXTRACTOR_LIB_HDRS
  analysis.h
  chain_extractor.h
  options.h
  dependency_graph.h
  chain_pass.h
  chain_cache.h
  parser_deps.h
  post_midend.h
  p4runtime_translation.h
)

# Linkable library for external consumers (e.g. p4tools/symbex).
add_library(p4ccx-lib STATIC ${CHAIN_EXTRACTOR_LIB_SRCS})
target_link_libraries(p4ccx-lib PUBLIC p4ifds-lib ${P4C_LIBRARIES} ${P4C_LIB_DEPS})
add_dependencies(p4ccx-lib ir-generated frontend)
target_include_directories(p4ccx-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Standalone binary keeps its own main() + options.
add_executable(p4c_chain_extractor
  chain_extractor.cpp
  options.cpp
  post_midend.cpp
  p4runtime_translation.cpp
  # --post-midend reuses p4symbex's midend pass list verbatim instead of copying it, so the two
  # can never drift. These TUs depend only on core p4c (no Z3, no Target registry, no
  # CompilerResult), so no p4tools library is linked and ENABLE_P4TOOLS is irrelevant here.
  ${P4C_SOURCE_DIR}/backends/p4tools/common/compiler/midend.cpp
  ${P4C_SOURCE_DIR}/backends/p4tools/common/compiler/convert_varbits.cpp
  ${P4C_SOURCE_DIR}/backends/p4tools/common/compiler/convert_struct_expr.cpp
  ${EXTENSION_P4_14_CONV_SOURCES}
)
target_link_libraries(p4c_chain_extractor p4ccx-lib ${P4C_LIBRARIES} ${P4C_LIB_DEPS})
add_dependencies(p4c_chain_extractor ir-generated frontend)

install(TARGETS p4c_chain_extractor
  RUNTIME DESTINATION ${P4C_RUNTIME_OUTPUT_DIRECTORY})

file(RELATIVE_PATH
  CURRENT_BINARY_DIR_PATH_REL
  ${P4C_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

# This link is not required but it is convenient to have access to this
# backend in the top-level build directory, along with all the other backends.
add_custom_target(linkchain_extractor
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CURRENT_BINARY_DIR_PATH_REL}/p4c_chain_extractor ${P4C_BINARY_DIR}/p4c_chain_extractor
  DEPENDS update_includes
  )
add_dependencies(p4c_driver linkchain_extractor)
