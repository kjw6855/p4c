# IFDS/IDE solver library shared by the state_dependency and chain_extractor backends:
# ICFG construction (controls/parsers), exploded supergraph, tabulation solver, IDEPass base.
set (IFDS_LIB_SRCS
  graphs.cpp
  controls.cpp
  parsers.cpp
  graph_visitor.cpp
  supergraphs.cpp
  tabulation.cpp
  ide_pass.cpp
  utils.cpp
  edge_func_provider.cpp
  flow_overlay.cpp
)

set (IFDS_LIB_HDRS
  graphs.h
  controls.h
  parsers.h
  graph_visitor.h
  supergraphs.h
  tabulation.h
  ide_pass.h
  utils.h
  edge_func_provider.h
  flow_overlay.h
)

add_library(p4ifds-lib STATIC ${IFDS_LIB_SRCS})
target_link_libraries(p4ifds-lib PUBLIC ${P4C_LIBRARIES} ${P4C_LIB_DEPS})
add_dependencies(p4ifds-lib ir-generated frontend)
