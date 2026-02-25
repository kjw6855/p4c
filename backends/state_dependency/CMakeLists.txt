configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.h.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

set (STATE_DEPENDENCY_SRCS
  state_dependency.cpp
  controls.cpp
  parsers.cpp
  options.cpp
  graphs.cpp
  graph_visitor.cpp
  supergraphs.cpp
)

set (STATE_DEPENDENCY_HDRS
  state_dependency.h
  controls.h
  parsers.h
  options.h
  graphs.h
  graph_visitor.h
  supergraphs.h
)

add_executable(p4c_state_dependency ${STATE_DEPENDENCY_SRCS} ${EXTENSION_P4_14_CONV_SOURCES})
target_link_libraries(p4c_state_dependency ${P4C_LIBRARIES} ${P4C_LIB_DEPS})
add_dependencies(p4c_state_dependency ir-generated frontend)

install(TARGETS p4c_state_dependency
  RUNTIME DESTINATION ${P4C_RUNTIME_OUTPUT_DIRECTORY})

file(RELATIVE_PATH
  CURRENT_BINARY_DIR_PATH_REL
  ${P4C_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

# This link is not required but it is convenient to have access to this
# backend in the top-level build directory, along with all the other backends.
add_custom_target(linkstate_dependency
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CURRENT_BINARY_DIR_PATH_REL}/p4c_state_dependency ${P4C_BINARY_DIR}/p4c_state_dependency
  DEPENDS update_includes
  )
add_dependencies(p4c_driver linkstate_dependency)

