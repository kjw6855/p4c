if(ENABLE_TESTING)
  # Include the test subdirectory.
  message("-- Adding p4fuzzer ebpf test suite")
  include(test/P4Tests.cmake)
endif()

# Source files for p4fuzzer.
set(
  FUZZER_SOURCES
  ${FUZZER_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/stf/stf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ebpf.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cmd_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/concolic.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/constants.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/expr_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/program_info.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/table_stepper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/target.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_spec.cpp
  PARENT_SCOPE
)

# Link the run-ebpf-test binary
execute_process(COMMAND ln -sfn ${P4C_SOURCE_DIR}/backends/ebpf/run-ebpf-test.py ${CMAKE_BINARY_DIR}/run-ebpf-test.py)

set(
  FUZZER_LIBS ${FUZZER_LIBS}
  ${P4C_LIBRARIES}
  ${P4C_LIB_DEPS}
  ${CMAKE_THREAD_LIBS_INIT}
  PARENT_SCOPE
)
