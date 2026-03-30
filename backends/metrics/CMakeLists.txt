# CMakefile for the Metrics P4-16 back-end.
message(STATUS "Start configuring the metrics back end")

set (P4C_METRICS_SRCS
  p4c-metrics.cpp
  metricsOptions.cpp
  metricsPassManager.cpp
  linesOfCodeMetric.cpp
  actionParameterMetrics.cpp
  cyclomaticComplexity.cpp
  unusedCodeMetric.cpp
  nestingDepthMetric.cpp
  halsteadMetrics.cpp
  headerMetrics.cpp
  headerPacketMetrics.cpp
  matchActionTableMetrics.cpp
  parserMetrics.cpp
  inlinedActionsMetric.cpp
  externalObjectsMetric.cpp
  exportMetrics.cpp
)

set (P4C_METRICS_HDRS
  metricsOptions.h
  metricsPassManager.h
  linesOfCodeMetric.h
  actionParameterMetrics.h
  cyclomaticComplexity.h
  unusedCodeMetric.h
  nestingDepthMetric.h
  halsteadMetrics.h
  headerMetrics.h
  headerPacketMetrics.h
  matchActionTableMetrics.h
  parserMetrics.h
  inlinedActionsMetric.h
  externalObjectsMetric.h
  exportMetrics.h
)

add_executable(p4c-metrics ${P4C_METRICS_SRCS})
target_link_libraries (p4c-metrics ${P4C_LIBRARIES} ${P4C_LIB_DEPS})
add_dependencies(p4c-metrics ir-generated frontend)

install (TARGETS p4c-metrics
  RUNTIME DESTINATION ${P4C_RUNTIME_OUTPUT_DIRECTORY})

file(RELATIVE_PATH
  CURRENT_BINARY_DIR_PATH_REL
  ${P4C_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

# hack to get around the fact that the test scripts expect the backend
# binary to be in the top level directory. This should go away when we
# remove automake and fix the scripts.
add_custom_target(linkp4cmetrics
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CURRENT_BINARY_DIR_PATH_REL}/p4c-metrics ${P4C_BINARY_DIR}/p4c-metrics
)
add_dependencies(p4c_driver linkp4cmetrics)