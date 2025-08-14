macro(p4c_obtain_protobuf)
  option(
    P4C_USE_PREINSTALLED_PROTOBUF
    "Look for a preinstalled version of Protobuf in the system instead of installing a prebuilt binary using FetchContent."
    OFF
  )

  # If P4C_USE_PREINSTALLED_PROTOBUF is ON just try to find a preinstalled version of Protobuf.
  if(P4C_USE_PREINSTALLED_PROTOBUF)
    set(P4C_PROTOBUF_VERSION 25.3.0)
    if(ENABLE_PROTOBUF_STATIC)
      set(SAVED_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
      set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    # For MacOS, we may need to look for Protobuf in additional folders.
    if(APPLE)
      set(P4C_PROTOBUF_PATHS PATHS /usr/local/opt/protobuf /opt/homebrew/opt/protobuf)
    endif()
    # We do not set a minimum version here because Protobuf does not accept mismatched major versions.
    # We recommend the current P4C_PROTOBUF_VERSION.
    find_package(Protobuf ${P4C_PROTOBUF_VERSION} CONFIG ${P4C_PROTOBUF_PATHS})
    if(NOT Protobuf_FOUND)
      find_package(Protobuf REQUIRED CONFIG ${P4C_PROTOBUF_PATHS})
      message(
        WARNING
          "Major Protobuf version does not match with the expected ${P4C_PROTOBUF_VERSION} version."
          " You may experience compatibility problems."
      )
    endif()

    # Protobuf sets the protoc binary to a generator expression "$<TARGET_FILE:protoc>", but we many
    # not be able to use this generator expression in some text-based test scripts. The reason is
    # that protoc is only evaluated at build time, not during generation of the test scripts. TODO:
    # Maybe we can improve these scripts somehow?
    find_program(Protobuf_PROTOC_EXECUTABLE protoc)

    if(ENABLE_PROTOBUF_STATIC)
      set(CMAKE_FIND_LIBRARY_SUFFIXES ${SAVED_CMAKE_FIND_LIBRARY_SUFFIXES})
    endif()
  else()
    set(P4C_PROTOBUF_VERSION 25.3)
    message(STATUS "Fetching Protobuf version ${P4C_PROTOBUF_VERSION} for P4C...")

    # Unity builds do not work for Protobuf...
    set(CMAKE_UNITY_BUILD_PREV ${CMAKE_UNITY_BUILD})
    set(CMAKE_UNITY_BUILD OFF)
    # Print out download state while setting up Protobuf.
    set(FETCHCONTENT_QUIET_PREV ${FETCHCONTENT_QUIET})
    set(FETCHCONTENT_QUIET OFF)
    # Build Protobuf with position-independent code.
    set(CMAKE_POSITION_INDEPENDENT_CODE_PREV ${CMAKE_POSITION_INDEPENDENT_CODE})
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    set(protobuf_BUILD_TESTS OFF CACHE BOOL "Build tests.")
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "Build libprotoc and protoc compiler.")
    # Only ever build the static library. It is not safe to link with a local dynamic version.
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "Build Shared Libraries")
    set(protobuf_LOCAL_INSTALL_DIR "${protobuf_BINARY_DIR}/protobuf_local_install" CACHE PATH "Local installation path for Protobuf")
    file(MAKE_DIRECTORY "${protobuf_LOCAL_INSTALL_DIR}")
    set(CMAKE_INSTALL_PREFIX ${protobuf_LOCAL_INSTALL_DIR} CACHE PATH "Install path")

    # Exclude Protobuf from the main make install step. We only want to use it locally.
    set(protobuf_INSTALL ON CACHE BOOL "Install Protobuf")
    set(protobuf_ABSL_PROVIDER "package" CACHE STRING "Use system-provided abseil")
    set(protobuf_BUILD_EXPORT ON)
    set(utf8_range_ENABLE_INSTALL ON)

    fetchcontent_declare(
      protobuf
      URL https://github.com/protocolbuffers/protobuf/releases/download/v${P4C_PROTOBUF_VERSION}/protobuf-${P4C_PROTOBUF_VERSION}.tar.gz
      URL_HASH SHA256=d19643d265b978383352b3143f04c0641eea75a75235c111cc01a1350173180e
      USES_TERMINAL_DOWNLOAD TRUE
      GIT_PROGRESS TRUE
      CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${protobuf_LOCAL_INSTALL_DIR}
        -Dprotobuf_BUILD_TESTS=OFF
        -Dprotobuf_BUILD_PROTOC_BINARIES=ON
        -Dprotobuf_BUILD_SHARED_LIBS=OFF # If you want static, ensure this is OFF
        -Dprotobuf_ABSL_PROVIDER=package
        -Dprotobuf_INSTALL=ON # Explicitly ensure install is ON for the fetched content
    )
    fetchcontent_makeavailable(protobuf)

    # Build and install Protobuf to generate ProtobufConfig.cmake
    add_custom_target(protobuf_install ALL
      COMMAND ${CMAKE_COMMAND} --build . --target install
      WORKING_DIRECTORY ${protobuf_BINARY_DIR}
      COMMENT "Installing Protobuf to ${protobuf_LOCAL_INSTALL_DIR}"
    )

    # Protobuf and protoc source code may trigger warnings which we ignore.
    set_target_properties(libprotobuf-lite PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    set_target_properties(libprotobuf PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    set_target_properties(libprotoc PROPERTIES COMPILE_FLAGS "-Wno-error -w")

    find_package(utf8_range)

    # Set some Protobuf variables manually until we are able to call FindPackage directly. This
    # should be possible with CMake 3.24. Protobuf sets the protoc binary to a generator expression
    # "$<TARGET_FILE:protoc>", but we many not be able to use this generator expression in some
    # text-based test scripts. The reason is that protoc is only evaluated at build time, not during
    # generation of the test scripts. TODO: Maybe we can improve these scripts somehow?
    set(Protobuf_PROTOC_EXECUTABLE ${protobuf_BINARY_DIR}/protoc)
    set(Protobuf_DIR "${protobuf_LOCAL_INSTALL_DIR}/lib/cmake/protobuf" CACHE PATH "Path to ProtobufConfig.cmake")
    include(${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake)
    # Protobuf does not seem to set Protobuf_INCLUDE_DIRS correctly when used as a module, but we
    # need this variable for generating code.
    list(APPEND Protobuf_INCLUDE_DIRS "${protobuf_SOURCE_DIR}/src/")

    # Reset temporary variable modifications.
    set(CMAKE_UNITY_BUILD ${CMAKE_UNITY_BUILD_PREV})
    set(FETCHCONTENT_QUIET ${FETCHCONTENT_QUIET_PREV})
    set(CMAKE_POSITION_INDEPENDENT_CODE ${CMAKE_POSITION_INDEPENDENT_CODE_PREV})
  endif()

  message(STATUS "Done with setting up Protobuf for P4C.")
endmacro(p4c_obtain_protobuf)
