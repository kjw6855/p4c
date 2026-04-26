macro(p4c_obtain_grpc)
  set(P4C_GRPC_VERSION "1.60.0")
  option(
    P4C_USE_PREINSTALLED_GRPC
    "Look for a preinstalled version of gRPC in the system instead of fetching via FetchContent."
    OFF
  )

  if(P4C_USE_PREINSTALLED_GRPC)
    find_package(gRPC REQUIRED CONFIG)
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
    message(STATUS "Found preinstalled gRPC: ${gRPC_VERSION}")
  else()
    message(STATUS "Fetching gRPC version ${P4C_GRPC_VERSION} for P4C...")

    set(FETCHCONTENT_QUIET_PREV ${FETCHCONTENT_QUIET})
    set(FETCHCONTENT_QUIET OFF)
    set(CMAKE_UNITY_BUILD_PREV ${CMAKE_UNITY_BUILD})
    set(CMAKE_UNITY_BUILD OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE_PREV ${CMAKE_POSITION_INDEPENDENT_CODE})
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    # ---------------------------------------------------------------------------
    # Create a protobuf shim directory for gRPC's module-mode protobuf provider.
    #
    # gRPC's default (module) mode calls:
    #   add_subdirectory(${PROTOBUF_ROOT_DIR} third_party/protobuf)
    # then checks "if(TARGET libprotobuf)".
    #
    # Since p4c already built protobuf via FetchContent (libprotobuf, libprotoc,
    # protoc targets are all in scope), we point PROTOBUF_ROOT_DIR at a stub
    # CMakeLists.txt that adds no new targets.  gRPC's TARGET checks then find
    # the FetchContent-built targets transparently.
    #
    # We also symlink the real protobuf src/ so gRPC can locate well-known protos.
    # ---------------------------------------------------------------------------
    set(_GRPC_PROTOBUF_SHIM "${CMAKE_CURRENT_BINARY_DIR}/grpc-protobuf-shim")
    file(MAKE_DIRECTORY "${_GRPC_PROTOBUF_SHIM}")
    file(WRITE "${_GRPC_PROTOBUF_SHIM}/CMakeLists.txt"
      "cmake_minimum_required(VERSION 3.10)\nproject(protobuf)\n"
      "# Stub: libprotobuf/libprotoc/protoc are already built by p4c FetchContent.\n"
    )
    # Symlink src/ so _gRPC_PROTOBUF_WELLKNOWN_INCLUDE_DIR resolves correctly.
    if(NOT EXISTS "${_GRPC_PROTOBUF_SHIM}/src")
      execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "${protobuf_SOURCE_DIR}/src"
          "${_GRPC_PROTOBUF_SHIM}/src"
      )
    endif()
    set(PROTOBUF_ROOT_DIR "${_GRPC_PROTOBUF_SHIM}" CACHE PATH
        "Protobuf root for gRPC module provider" FORCE)

    # ---------------------------------------------------------------------------
    # For abseil, gRPC's cmake has an early-exit:
    #   if(TARGET absl::strings)  # skip everything
    # The FetchContent-built abseil already provides these targets, so no action
    # needed — just leave gRPC_ABSL_PROVIDER at its default ("module").
    # ---------------------------------------------------------------------------

    # ---------------------------------------------------------------------------
    # gRPC build options.
    # ---------------------------------------------------------------------------
    set(gRPC_BUILD_TESTS        OFF CACHE BOOL "Build gRPC tests" FORCE)
    set(gRPC_BUILD_CSHARP_EXT   OFF CACHE BOOL "" FORCE)
    set(gRPC_INSTALL            OFF CACHE BOOL "Install gRPC" FORCE)
    set(gRPC_BUILD_GRPC_PYTHON_PLUGIN      OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_RUBY_PLUGIN        OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_PHP_PLUGIN         OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_NODE_PLUGIN        OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_CPP_PLUGIN         ON  CACHE BOOL "" FORCE)

    # Use module mode (default): gRPC will add_subdirectory the shim above and
    # then find the already-existing libprotobuf/libprotoc/protoc targets.
    set(gRPC_PROTOBUF_PROVIDER module CACHE STRING "" FORCE)
    # Leave gRPC_ABSL_PROVIDER at its default ("module"); the early-exit
    # "if(TARGET absl::strings)" in gRPC's abseil-cpp.cmake handles it.

    set(gRPC_ZLIB_PROVIDER  module CACHE STRING "" FORCE)
    set(gRPC_CARES_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_RE2_PROVIDER   module CACHE STRING "" FORCE)
    set(gRPC_SSL_PROVIDER   module CACHE STRING "" FORCE)

    FetchContent_Declare(
      gRPC
      GIT_REPOSITORY https://github.com/grpc/grpc.git
      GIT_TAG        v${P4C_GRPC_VERSION}
      GIT_SHALLOW    ON
      GIT_PROGRESS   TRUE
    )
    FetchContent_MakeAvailable(gRPC)

    # Suppress warnings in gRPC sources.
    foreach(_grpc_target grpc grpc++ grpc++_reflection address_sorting upb)
      if(TARGET ${_grpc_target})
        set_target_properties(${_grpc_target} PROPERTIES COMPILE_FLAGS "-Wno-error -w")
      endif()
    endforeach()

    # Create gRPC:: namespace aliases so code that links against the conventional
    # gRPC::grpc++ / gRPC::gpr targets (as found by find_package) works with
    # FetchContent-built gRPC as well.
    if(TARGET grpc++ AND NOT TARGET gRPC::grpc++)
      add_library(gRPC::grpc++ ALIAS grpc++)
    endif()
    if(TARGET grpc AND NOT TARGET gRPC::grpc)
      add_library(gRPC::grpc ALIAS grpc)
    endif()
    if(TARGET gpr AND NOT TARGET gRPC::gpr)
      add_library(gRPC::gpr ALIAS gpr)
    endif()
    if(TARGET grpc++_reflection AND NOT TARGET gRPC::grpc++_reflection)
      add_library(gRPC::grpc++_reflection ALIAS grpc++_reflection)
    endif()

    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

    set(CMAKE_UNITY_BUILD ${CMAKE_UNITY_BUILD_PREV})
    set(FETCHCONTENT_QUIET ${FETCHCONTENT_QUIET_PREV})
    set(CMAKE_POSITION_INDEPENDENT_CODE ${CMAKE_POSITION_INDEPENDENT_CODE_PREV})
  endif()

  message(STATUS "Done setting up gRPC for P4C.")
endmacro(p4c_obtain_grpc)
