cmake_minimum_required(VERSION 3.16)

project(Matrix LANGUAGES C CXX)

include(FetchContent)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emit build/compile_commands.json for clangd/ccls. A symlink at the repo root
# (gitignored) is refreshed on every configure so it never goes stale when the
# build directory moves.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MATRIX_COMPILE_COMMANDS_LINK "${CMAKE_SOURCE_DIR}/compile_commands.json")
  if(NOT EXISTS "${MATRIX_COMPILE_COMMANDS_LINK}" OR IS_SYMLINK "${MATRIX_COMPILE_COMMANDS_LINK}")
    file(REMOVE "${MATRIX_COMPILE_COMMANDS_LINK}")
    file(CREATE_LINK
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${MATRIX_COMPILE_COMMANDS_LINK}"
      SYMBOLIC)
  endif()
endif()

find_package(PkgConfig REQUIRED)
find_package(Git QUIET)
find_program(PROTOC_EXECUTABLE protoc REQUIRED)

option(MATRIX_FORCE_FETCH_CROW "Build Crow from source even when a system package is installed" OFF)
option(MATRIX_FETCH_CROW "Allow downloading Crow from the network when no system package is found" OFF)
option(MATRIX_ENABLE_WEBGUI "Build the multiplayer web GUI (requires Crow)" ON)
option(MATRIX_WARNINGS "Enable -Wall -Wextra on all targets" ON)
option(MATRIX_NATIVE_ARCH "Optimize for the build machine CPU (-march=native)" OFF)
option(MATRIX_BUILD_TESTS "Build the gtest suites" ON)
option(MATRIX_ENABLE_INTEGRATION_TESTS
  "Register compositor-spawning integration tests with CTest (needs a Wayland/DRM session)" OFF)
set(MATRIX_SANITIZE "" CACHE STRING
  "Comma-separated sanitizers to enable, e.g. \"address,undefined\" (recommended with Debug)")
set(MATRIX_CROW_GIT_REPOSITORY "https://github.com/CrowCpp/Crow.git" CACHE STRING
  "Crow git repository used when fetching from source")
set(MATRIX_CROW_GIT_TAG "c61a26e" CACHE STRING
  "Pinned Crow git revision used when fetching from source")
set(MATRIX_CROW_SOURCE_DIR "" CACHE PATH
  "Existing Crow source checkout to use instead of downloading Crow")

# Match the previous Release behaviour (-O3 with debug info; CMake still adds
# -DNDEBUG from CMAKE_CXX_FLAGS_RELEASE) while giving Debug builds a real -O0.
# Previously -O3 was appended unconditionally, so Debug was silently -O3 too.
set(MATRIX_OPT_FLAGS
  "$<$<CONFIG:Debug>:-O0>"
  "$<$<CONFIG:Debug>:-g>"
  "$<$<NOT:$<CONFIG:Debug>>:-O3>"
  "$<$<NOT:$<CONFIG:Debug>>:-g>")

# Sanitizers (e.g. -DMATRIX_SANITIZE=address,undefined). Compile flags go to the
# target itself; link flags are PUBLIC so consumers (executables/tests) pick up
# the runtime as well.
function(matrix_apply_sanitizers target)
  if(NOT MATRIX_SANITIZE)
    return()
  endif()
  target_compile_options(${target} PRIVATE
    "-fsanitize=${MATRIX_SANITIZE}"
    -fno-omit-frame-pointer)
  target_link_options(${target} PUBLIC "-fsanitize=${MATRIX_SANITIZE}")
endfunction()

function(configure_crow_source_options)
  set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "Disable Crow examples when used as a dependency" FORCE)
  set(CROW_BUILD_TESTS OFF CACHE BOOL "Disable Crow tests when used as a dependency" FORCE)
  set(CROW_BUILD_FUZZER OFF CACHE BOOL "Disable Crow fuzzer when used as a dependency" FORCE)
  set(CROW_INSTALL OFF CACHE BOOL "Disable Crow install rules when used as a dependency" FORCE)
  set(CROW_ENABLE_SSL OFF CACHE BOOL "Disable optional Crow SSL support for HackMatrix" FORCE)
  set(CROW_ENABLE_COMPRESSION OFF CACHE BOOL "Disable optional Crow compression support for HackMatrix" FORCE)
endfunction()

# Crow resolution order:
#   1. system package (find_package)          <- preferred
#   2. MATRIX_CROW_SOURCE_DIR local checkout  <- offline builds
#   3. network fetch, only if MATRIX_FETCH_CROW=ON
#   4. otherwise disable the optional WebGui instead of failing configure
set(MATRIX_CROW_RESOLVED OFF)

if(MATRIX_ENABLE_WEBGUI AND NOT MATRIX_FORCE_FETCH_CROW)
  find_package(Crow CONFIG QUIET)
  if(TARGET Crow::Crow)
    set(MATRIX_CROW_RESOLVED ON)
    message(STATUS "Crow: using system package")
  endif()
endif()

if(NOT MATRIX_CROW_RESOLVED AND MATRIX_ENABLE_WEBGUI)
  configure_crow_source_options()

  if(MATRIX_CROW_SOURCE_DIR)
    if(NOT EXISTS "${MATRIX_CROW_SOURCE_DIR}/CMakeLists.txt")
      message(FATAL_ERROR
        "MATRIX_CROW_SOURCE_DIR was set to '${MATRIX_CROW_SOURCE_DIR}', but that directory does not contain a CMakeLists.txt.")
    endif()

    message(STATUS "Crow: using local source tree ${MATRIX_CROW_SOURCE_DIR}")
    add_subdirectory("${MATRIX_CROW_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/crow-build" EXCLUDE_FROM_ALL)
    set(MATRIX_CROW_RESOLVED ON)
  elseif(MATRIX_FETCH_CROW)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "Crow was not found and Git is unavailable. Install Crow, or configure with MATRIX_CROW_SOURCE_DIR to point at a Crow checkout.")
    endif()

    message(STATUS "Crow: fetching ${MATRIX_CROW_GIT_REPOSITORY} at ${MATRIX_CROW_GIT_TAG}")
    FetchContent_Declare(crow
      GIT_REPOSITORY "${MATRIX_CROW_GIT_REPOSITORY}"
      GIT_TAG "${MATRIX_CROW_GIT_TAG}"
      GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(crow)
    set(MATRIX_CROW_RESOLVED ON)
  endif()
endif()

if(MATRIX_ENABLE_WEBGUI AND NOT MATRIX_CROW_RESOLVED)
  message(WARNING
    "Crow was not found, so the multiplayer WebGui is disabled.\n"
    "  * install a system Crow package, or\n"
    "  * configure with -DMATRIX_FETCH_CROW=ON to download it, or\n"
    "  * configure with -DMATRIX_CROW_SOURCE_DIR=/path/to/crow for an offline build.\n"
    "Set -DMATRIX_ENABLE_WEBGUI=OFF to silence this warning.")
  set(MATRIX_ENABLE_WEBGUI OFF)
endif()

if(MATRIX_ENABLE_WEBGUI)
  if(NOT TARGET Crow::Crow)
    message(FATAL_ERROR "Crow dependency setup failed to provide the Crow::Crow target.")
  endif()
  message(STATUS "WebGui: enabled (Crow)")
else()
  message(STATUS "WebGui: disabled")
endif()

# wlroots: prefer a system install, fall back to building the submodule.
#
# Building the submodule runs meson+ninja *at configure time*, which makes
# `cmake` slow and requires a working toolchain before a build system even
# exists. A system wlroots avoids all of that.
option(MATRIX_USE_SYSTEM_WLROOTS "Use a system wlroots instead of building the submodule" ON)
option(MATRIX_FORCE_BUILD_WLROOTS "Build the bundled wlroots submodule even if a system one exists" OFF)

set(WLROOTS_DIR "${CMAKE_SOURCE_DIR}/wlroots")
set(WLROOTS_BUILD_DIR "${WLROOTS_DIR}/build")
set(WLROOTS_PC_DIR "${WLROOTS_BUILD_DIR}/meson-uninstalled")
set(WLROOTS_PROTOCOL_DIR "${WLROOTS_BUILD_DIR}/protocol")

set(WLROOTS_PC_NAME "wlroots-0.19")
find_program(MESON_EXECUTABLE meson)
find_program(NINJA_EXECUTABLE ninja)

# True when a usable system wlroots-0.19.pc is visible to pkg-config.
set(WLROOTS_SYSTEM_AVAILABLE OFF)
if(MATRIX_USE_SYSTEM_WLROOTS AND NOT MATRIX_FORCE_BUILD_WLROOTS)
  pkg_check_modules(WLROOTS_SYSTEM QUIET wlroots-0.19)
  if(WLROOTS_SYSTEM_FOUND)
    set(WLROOTS_SYSTEM_AVAILABLE ON)
  endif()
endif()

# The compositor includes wlroots *private* headers (struct wlr_wl_output, ...)
# which distros never install. Those come from the submodule, so the submodule
# version must match the system library or the field offsets can silently differ.
option(MATRIX_STRICT_WLROOTS_VERSION
  "Fail instead of warn when the system wlroots version differs from the submodule" OFF)
if(WLROOTS_SYSTEM_AVAILABLE AND EXISTS "${WLROOTS_DIR}/meson.build")
  file(STRINGS "${WLROOTS_DIR}/meson.build" _wlroots_version_line
    REGEX "version: '[0-9]+\\.[0-9]+\\.[0-9]+'" LIMIT_COUNT 1)
  if(_wlroots_version_line MATCHES "version: '([0-9]+\\.[0-9]+\\.[0-9]+)'")
    set(WLROOTS_VENDORED_VERSION "${CMAKE_MATCH_1}")
    if(NOT WLROOTS_SYSTEM_VERSION STREQUAL WLROOTS_VENDORED_VERSION)
      string(CONCAT _wlroots_mismatch_msg
        "system wlroots is ${WLROOTS_SYSTEM_VERSION} but the private headers in wlroots/ are ${WLROOTS_VENDORED_VERSION}.\n"
        "  The compositor reads wlroots private struct fields, so a version\n"
        "  mismatch can cause memory corruption. Pin the submodule to the same\n"
        "  version, or configure with -DMATRIX_USE_SYSTEM_WLROOTS=OFF.")
      if(MATRIX_STRICT_WLROOTS_VERSION)
        message(FATAL_ERROR "${_wlroots_mismatch_msg}")
      else()
        message(WARNING "${_wlroots_mismatch_msg}")
      endif()
    endif()
  endif()
endif()

# Build wlroots locally via meson/ninja, matching the makefile flow, and bootstrap the submodule if missing.
# Skipped entirely when a system wlroots is being used.
function(build_wlroots_if_needed)
  if(WLROOTS_SYSTEM_AVAILABLE)
    message(STATUS "Using system wlroots (${WLROOTS_SYSTEM_VERSION}); skipping submodule build")
    return()
  endif()

  # Only skip the build when the library itself exists. The meson-generated
  # .pc file appears at *configure* time, so checking it alone would accept a
  # half-built tree after an interrupted build.
  if(EXISTS "${WLROOTS_BUILD_DIR}/libwlroots-0.19.so" AND
     (EXISTS "${WLROOTS_PC_DIR}/wlroots-0.19-uninstalled.pc" OR EXISTS "${WLROOTS_PC_DIR}/wlroots-0.19.pc"))
    return()
  endif()

  if(NOT MESON_EXECUTABLE OR NOT NINJA_EXECUTABLE)
    message(FATAL_ERROR "Meson and Ninja are required to build wlroots automatically.")
  endif()

  if(NOT EXISTS "${WLROOTS_DIR}/meson.build")
    if(NOT GIT_FOUND)
      message(FATAL_ERROR "Git is required to initialize the wlroots submodule automatically.")
    endif()
    message(STATUS "Initializing wlroots submodule")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive wlroots
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE WLROOTS_GIT_RESULT
    )
    if(NOT WLROOTS_GIT_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to initialize wlroots submodule (exit code ${WLROOTS_GIT_RESULT}).")
    endif()
  endif()

  message(STATUS "Configuring wlroots with meson")
  # `meson setup` refuses to run twice on the same build dir, so reconfigure
  # when it already exists instead of failing on the second cmake run.
  set(WLROOTS_MESON_ARGS setup build -Dexamples=false -Dwerror=false)
  if(EXISTS "${WLROOTS_DIR}/build/build.ninja")
    set(WLROOTS_MESON_ARGS setup --reconfigure build -Dexamples=false -Dwerror=false)
  endif()
  execute_process(
    COMMAND "${MESON_EXECUTABLE}" ${WLROOTS_MESON_ARGS}
    WORKING_DIRECTORY "${WLROOTS_DIR}"
    RESULT_VARIABLE WLROOTS_MESON_RESULT
  )
  if(NOT WLROOTS_MESON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to configure wlroots with meson (exit code ${WLROOTS_MESON_RESULT}).")
  endif()

  message(STATUS "Building wlroots with ninja")
  execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C build
    WORKING_DIRECTORY "${WLROOTS_DIR}"
    RESULT_VARIABLE WLROOTS_NINJA_RESULT
  )
  if(NOT WLROOTS_NINJA_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to build wlroots with ninja (exit code ${WLROOTS_NINJA_RESULT}).")
  endif()
endfunction()

build_wlroots_if_needed()

if(WLROOTS_SYSTEM_AVAILABLE)
  # System install: no local protocol dir, no PKG_CONFIG_PATH override.
  # Keep the same module list as the bundled path so link flags match.
  pkg_check_modules(WLROOTS_LOCAL REQUIRED wlroots-0.19 wayland-server wayland-client xkbcommon)
  set(WLROOTS_PC_NAME "wlroots-0.19")
  set(WLROOTS_PROTOCOL_DIR "")
  # wlroots never installs its private backend headers, but this compositor
  # uses private types. They are plain headers in the submodule, so adding the
  # include dir costs nothing and avoids building the submodule entirely.
  set(WLROOTS_PRIVATE_INCLUDE_DIR "${WLROOTS_DIR}/include")
else()
  if(EXISTS "${WLROOTS_PC_DIR}/wlroots-0.19-uninstalled.pc")
    set(WLROOTS_PC_NAME "wlroots-0.19-uninstalled")
  elseif(EXISTS "${WLROOTS_PC_DIR}/wlroots-0.19.pc")
    set(WLROOTS_PC_NAME "wlroots-0.19")
  endif()
  if(EXISTS "${WLROOTS_PC_DIR}")
    set(ENV{PKG_CONFIG_PATH} "${WLROOTS_PC_DIR}:$ENV{PKG_CONFIG_PATH}")
  endif()

  pkg_check_modules(WLROOTS_LOCAL REQUIRED ${WLROOTS_PC_NAME} wayland-server wayland-client xkbcommon)
endif()

# --- Generate missing wlroots protocol headers -------------------------------
# Distribution wlroots packages (e.g. Arch's wlroots0.19) ship public headers
# that #include "<name>-protocol.h" without shipping those generated files.
# Generate whatever is missing from the xml definitions bundled in wlroots/protocol.
set(MATRIX_PROTOCOL_GEN_DIR "${CMAKE_BINARY_DIR}/generated/protocol")
set(MATRIX_GENERATED_PROTOCOL_HEADERS "")

function(matrix_generate_missing_protocol_headers)
  find_program(WAYLAND_SCANNER wayland-scanner)
  if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner is required to generate wlroots protocol headers.")
  endif()

  set(PROTO_XML_DIR "${CMAKE_SOURCE_DIR}/wlroots/protocol")

  # wlroots public headers live in the first include dir of the .pc file.
  set(wlroots_include_dir "")
  if(WLROOTS_LOCAL_INCLUDE_DIRS)
    list(GET WLROOTS_LOCAL_INCLUDE_DIRS 0 wlroots_include_dir)
  endif()
  if(NOT wlroots_include_dir OR NOT EXISTS "${wlroots_include_dir}")
    message(STATUS "No wlroots include dir found; skipping protocol header generation")
    return()
  endif()

  file(GLOB_RECURSE WLROOTS_PUBLIC_HEADERS "${wlroots_include_dir}/wlr/*.h")

  # Also scan project sources: they may include the client variant directly.
  file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/include/*.h"
  )

  # Protocol xml lives either in the bundled wlroots/protocol dir (wlr-* and
  # a few others) or in the system wayland-protocols tree (xdg-shell etc).
  find_path(WAYLAND_PROTOCOLS_DIR
    NAMES stable/xdg-shell/xdg-shell.xml
    PATHS /usr/share/wayland-protocols /usr/local/share/wayland-protocols
  )

  set(missing "")
  foreach(src IN LISTS WLROOTS_PUBLIC_HEADERS PROJECT_SOURCES)
    file(STRINGS "${src}" refs REGEX "#include \"?[a-z0-9-]+-protocol\\.h")
    foreach(ref IN LISTS refs)
      string(REGEX MATCH "[a-z0-9-]+-protocol\\.h" hdr_name "${ref}")
      if(hdr_name)
        list(APPEND missing "${hdr_name}")
      endif()
    endforeach()
  endforeach()

  if(NOT missing)
    return()
  endif()
  list(REMOVE_DUPLICATES missing)

  # Normalize both <base>-protocol.h and <base>-client-protocol.h to <base>.xml
  set(xml_names "")
  foreach(hdr IN LISTS missing)
    string(REGEX REPLACE "(-client)?-protocol\\.h$" "" base "${hdr}")
    list(APPEND xml_names "${base}.xml")
  endforeach()
  list(REMOVE_DUPLICATES xml_names)

  set(generated "")
  foreach(xml_name IN LISTS xml_names)
    set(xml_path "")
    if(EXISTS "${PROTO_XML_DIR}/${xml_name}")
      set(xml_path "${PROTO_XML_DIR}/${xml_name}")
    elseif(WAYLAND_PROTOCOLS_DIR)
      file(GLOB_RECURSE system_xml "${WAYLAND_PROTOCOLS_DIR}/*/${xml_name}")
      if(system_xml)
        list(GET system_xml 0 xml_path)
      endif()
    endif()

    if(NOT xml_path)
      message(WARNING "No xml found for ${xml_name}; compilation may fail")
      continue()
    endif()

    # wlroots names the server header  <base>-protocol.h
    # and the client header            <base>-client-protocol.h
    string(REGEX REPLACE "\\.xml$" "-protocol.h" server_hdr "${xml_name}")
    set(server_out "${MATRIX_PROTOCOL_GEN_DIR}/${server_hdr}")
    add_custom_command(
      OUTPUT "${server_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MATRIX_PROTOCOL_GEN_DIR}"
      COMMAND "${WAYLAND_SCANNER}" server-header "${xml_path}" "${server_out}"
      DEPENDS "${xml_path}"
      COMMENT "Generating ${server_hdr}"
      VERBATIM
    )
    list(APPEND generated "${server_out}")

    string(REGEX REPLACE "\\.xml$" "-client-protocol.h" client_hdr "${xml_name}")
    set(client_out "${MATRIX_PROTOCOL_GEN_DIR}/${client_hdr}")
    add_custom_command(
      OUTPUT "${client_out}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${MATRIX_PROTOCOL_GEN_DIR}"
      COMMAND "${WAYLAND_SCANNER}" client-header "${xml_path}" "${client_out}"
      DEPENDS "${xml_path}"
      COMMENT "Generating ${client_hdr}"
      VERBATIM
    )
    list(APPEND generated "${client_out}")
  endforeach()

  if(generated)
    add_custom_target(generate_protocol_headers DEPENDS ${generated})
    set(MATRIX_GENERATED_PROTOCOL_HEADERS ${generated} PARENT_SCOPE)
    set(MATRIX_PROTOCOL_GEN_DIR "${MATRIX_PROTOCOL_GEN_DIR}" PARENT_SCOPE)
    message(STATUS "wlroots protocol headers to generate: ${generated}")
  endif()
endfunction()

# --- Generate the private wlroots config.h ----------------------------------
# wlroots never installs its private "config.h" (meson generates it), but
# private headers such as backend/x11.h include it. One of the macros,
# HAVE_XCB_ERRORS, sits *inside* struct wlr_x11_backend, so getting it wrong
# silently breaks ABI. Derive it from the system wlroots .pc instead of
# hardcoding, so it always matches the library we link against.
function(matrix_generate_wlroots_private_config)
  if(NOT WLROOTS_PRIVATE_INCLUDE_DIR OR NOT EXISTS "${WLROOTS_PRIVATE_INCLUDE_DIR}")
    return()
  endif()

  # Does any private header we compile actually include "config.h"?
  file(GLOB_RECURSE private_headers "${WLROOTS_PRIVATE_INCLUDE_DIR}/backend/*.h")
  set(needs_config FALSE)
  foreach(hdr IN LISTS private_headers)
    file(STRINGS "${hdr}" has_config REGEX "^#include \"config\\.h\"")
    if(has_config)
      set(needs_config TRUE)
    endif()
  endforeach()
  if(NOT needs_config)
    return()
  endif()

  # Ask pkg-config how the *system* wlroots was built.
  execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --print-requires-private wlroots-0.19
    OUTPUT_VARIABLE wlroots_requires_private
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  set(have_xcb_errors 0)
  if(wlroots_requires_private MATCHES "xcb-errors")
    set(have_xcb_errors 1)
  endif()

  # Sanity check: the header must exist when we claim to support it.
  if(have_xcb_errors AND NOT EXISTS "/usr/include/xcb/xcb_errors.h")
    message(WARNING "wlroots was built with xcb-errors but xcb/xcb_errors.h is missing")
    set(have_xcb_errors 0)
  endif()

  set(config_out "${MATRIX_PROTOCOL_GEN_DIR}/config.h")
  file(WRITE "${config_out}"
"/* Generated by CMake for the private wlroots headers. Do not edit. */
#ifndef MATRIX_WLROOTS_PRIVATE_CONFIG_H
#define MATRIX_WLROOTS_PRIVATE_CONFIG_H

/* Must match the system libwlroots build; sits inside struct wlr_x11_backend. */
#define HAVE_XCB_ERRORS ${have_xcb_errors}

#endif
")
  # Must be written at configure time; no build rule needed.
  message(STATUS "wlroots private config.h: HAVE_XCB_ERRORS=${have_xcb_errors}")
endfunction()

if(WLROOTS_SYSTEM_AVAILABLE)
  matrix_generate_missing_protocol_headers()
  matrix_generate_wlroots_private_config()
endif()
pkg_check_modules(PROTOBUF REQUIRED protobuf)

execute_process(
  COMMAND "${PROTOC_EXECUTABLE}" --version
  OUTPUT_VARIABLE PROTOC_VERSION_OUTPUT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE PROTOC_VERSION_RESULT
)
if(NOT PROTOC_VERSION_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to query protoc version from ${PROTOC_EXECUTABLE}.")
endif()

string(REGEX REPLACE "^libprotoc " "" PROTOC_VERSION "${PROTOC_VERSION_OUTPUT}")
if(NOT PROTOC_VERSION VERSION_EQUAL PROTOBUF_VERSION)
  message(FATAL_ERROR
    "protoc (${PROTOC_VERSION}) does not match libprotobuf (${PROTOBUF_VERSION}). "
    "Use matching versions to avoid generated-code/runtime incompatibilities.")
endif()

set(PROTO_FILES
  "protos/api.proto"
)
set(PROTO_ABSOLUTE_FILES)
foreach(PROTO_FILE IN LISTS PROTO_FILES)
  list(APPEND PROTO_ABSOLUTE_FILES "${CMAKE_SOURCE_DIR}/${PROTO_FILE}")
endforeach()
set(PROTO_CPP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated")
set(PROTO_CPP_SOURCE "${PROTO_CPP_OUTPUT_DIR}/protos/api.pb.cc")
set(PROTO_CPP_HEADER "${PROTO_CPP_OUTPUT_DIR}/protos/api.pb.h")
set(PROTO_TMP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated-tmp")
set(PROTO_PYTHON_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/client_libs/python/hackMatrix")
set(PROTO_PYTHON_MODULE "${PROTO_PYTHON_OUTPUT_DIR}/protos/api_pb2.py")
set(PROTO_HASH_FILE "${CMAKE_BINARY_DIR}/generated/proto-inputs.sha256")
set(PROTO_STAMP_FILE "${CMAKE_BINARY_DIR}/generated/protos.stamp")
set(PROTO_GENERATOR_SCRIPT
  "${CMAKE_BINARY_DIR}/generated/generate_protos_if_changed.cmake")
set(PROTO_TOOLCHAIN_FINGERPRINT "protoc=${PROTOC_VERSION};libprotobuf=${PROTOBUF_VERSION}")

string(JOIN "\"\n  \"" PROTO_ABSOLUTE_FILES_LITERAL ${PROTO_ABSOLUTE_FILES})

set(PROTO_GENERATOR_SCRIPT_TEMPLATE [=[
set(PROTOC_EXECUTABLE "@PROTOC_EXECUTABLE@")
set(CMAKE_COMMAND_PATH "@CMAKE_COMMAND@")
set(CMAKE_SOURCE_DIR_PATH "@CMAKE_SOURCE_DIR@")
set(PROTO_CPP_OUTPUT_DIR "@PROTO_CPP_OUTPUT_DIR@")
set(PROTO_CPP_SOURCE "@PROTO_CPP_SOURCE@")
set(PROTO_CPP_HEADER "@PROTO_CPP_HEADER@")
set(PROTO_TMP_OUTPUT_DIR "@PROTO_TMP_OUTPUT_DIR@")
set(PROTO_PYTHON_OUTPUT_DIR "@PROTO_PYTHON_OUTPUT_DIR@")
set(PROTO_PYTHON_MODULE "@PROTO_PYTHON_MODULE@")
set(PROTO_HASH_FILE "@PROTO_HASH_FILE@")
set(PROTO_STAMP_FILE "@PROTO_STAMP_FILE@")
set(PROTO_TOOLCHAIN_FINGERPRINT "@PROTO_TOOLCHAIN_FINGERPRINT@")
set(PROTO_FILES
  "@PROTO_ABSOLUTE_FILES_LITERAL@"
)

set(PROTO_INPUT_HASH "toolchain:${PROTO_TOOLCHAIN_FINGERPRINT}\n")
foreach(PROTO_FILE IN LISTS PROTO_FILES)
  file(SHA256 "${PROTO_FILE}" PROTO_FILE_HASH)
  string(APPEND PROTO_INPUT_HASH "${PROTO_FILE}:${PROTO_FILE_HASH}\n")
endforeach()

set(SHOULD_GENERATE TRUE)
if(EXISTS "${PROTO_HASH_FILE}")
  file(READ "${PROTO_HASH_FILE}" PREVIOUS_PROTO_INPUT_HASH)
  if(PREVIOUS_PROTO_INPUT_HASH STREQUAL PROTO_INPUT_HASH
     AND EXISTS "${PROTO_CPP_SOURCE}"
     AND EXISTS "${PROTO_CPP_HEADER}"
     AND EXISTS "${PROTO_PYTHON_MODULE}")
    set(SHOULD_GENERATE FALSE)
  endif()
endif()

if(SHOULD_GENERATE)
  file(MAKE_DIRECTORY "${PROTO_CPP_OUTPUT_DIR}")
  file(MAKE_DIRECTORY "${PROTO_CPP_OUTPUT_DIR}/protos")
  file(MAKE_DIRECTORY "${PROTO_TMP_OUTPUT_DIR}")
  file(MAKE_DIRECTORY "${PROTO_TMP_OUTPUT_DIR}/protos")
  file(MAKE_DIRECTORY "${PROTO_PYTHON_OUTPUT_DIR}/protos")

  execute_process(
    COMMAND
      "${PROTOC_EXECUTABLE}"
      "--proto_path=${CMAKE_SOURCE_DIR_PATH}"
      "--cpp_out=${PROTO_TMP_OUTPUT_DIR}"
      ${PROTO_FILES}
    RESULT_VARIABLE PROTO_CPP_RESULT
  )
  if(NOT PROTO_CPP_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate protobuf C++ sources.")
  endif()

  execute_process(
    COMMAND
      "${PROTOC_EXECUTABLE}"
      "--proto_path=${CMAKE_SOURCE_DIR_PATH}"
      "--python_out=${PROTO_TMP_OUTPUT_DIR}"
      ${PROTO_FILES}
    RESULT_VARIABLE PROTO_PYTHON_RESULT
  )
  if(NOT PROTO_PYTHON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate protobuf Python sources.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api.pb.cc"
      "${PROTO_CPP_SOURCE}"
    RESULT_VARIABLE COPY_CPP_RESULT
  )
  if(NOT COPY_CPP_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf C++ source.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api.pb.h"
      "${PROTO_CPP_HEADER}"
    RESULT_VARIABLE COPY_HEADER_RESULT
  )
  if(NOT COPY_HEADER_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf C++ header.")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND_PATH}" -E copy_if_different
      "${PROTO_TMP_OUTPUT_DIR}/protos/api_pb2.py"
      "${PROTO_PYTHON_MODULE}"
    RESULT_VARIABLE COPY_PYTHON_RESULT
  )
  if(NOT COPY_PYTHON_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to update generated protobuf Python module.")
  endif()

  file(WRITE "${PROTO_HASH_FILE}" "${PROTO_INPUT_HASH}")
endif()

file(TOUCH "${PROTO_STAMP_FILE}")
]=])
string(CONFIGURE "${PROTO_GENERATOR_SCRIPT_TEMPLATE}" PROTO_GENERATOR_SCRIPT_CONTENT @ONLY)
file(WRITE "${PROTO_GENERATOR_SCRIPT}" "${PROTO_GENERATOR_SCRIPT_CONTENT}")

add_custom_command(
  OUTPUT
    "${PROTO_STAMP_FILE}"
  BYPRODUCTS
    "${PROTO_CPP_SOURCE}"
    "${PROTO_CPP_HEADER}"
    "${PROTO_PYTHON_MODULE}"
  COMMAND "${CMAKE_COMMAND}" -P "${PROTO_GENERATOR_SCRIPT}"
  DEPENDS
    ${PROTO_ABSOLUTE_FILES}
    "${PROTO_GENERATOR_SCRIPT}"
  COMMENT "Generating protobuf sources"
  VERBATIM
)

set_source_files_properties(
  "${PROTO_CPP_SOURCE}"
  "${PROTO_CPP_HEADER}"
  PROPERTIES GENERATED TRUE
)

add_custom_target(generate_protos DEPENDS
  "${PROTO_STAMP_FILE}"
)

# Build wlroots locally via meson/ninja, matching the makefile flow.
# Only when we are not using a system wlroots.
if(NOT WLROOTS_SYSTEM_AVAILABLE AND MESON_EXECUTABLE AND NINJA_EXECUTABLE)
  add_custom_command(
    OUTPUT "${WLROOTS_BUILD_DIR}/build.ninja"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/wlroots"
    COMMAND "${MESON_EXECUTABLE}" setup build -Dexamples=false -Dwerror=false
    COMMENT "Configuring wlroots with meson"
  )
  add_custom_command(
    OUTPUT "${WLROOTS_BUILD_DIR}/meson-info/intro-targets.json"
    DEPENDS "${WLROOTS_BUILD_DIR}/build.ninja"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/wlroots"
    COMMAND "${NINJA_EXECUTABLE}" -C build
    COMMENT "Building wlroots"
  )
  add_custom_target(wlroots_build DEPENDS "${WLROOTS_BUILD_DIR}/meson-info/intro-targets.json")
endif()

# Core sources mirroring the makefile's matrix target without enumerating every
# translation unit manually.
file(GLOB MATRIX_SOURCES CONFIGURE_DEPENDS
  src/*.cpp
  src/*.cc
  src/*.c
  src/components/*.cpp
  src/systems/*.cpp
  src/wayland/*.cpp
  src/MultiPlayer/*.cpp
  src/WindowManager/*.cpp
  src/Voxel/*.cpp
  src/imgui/*.cpp
  src/sqlite/*.cpp
)
file(GLOB TRACY_SOURCES CONFIGURE_DEPENDS tracy/public/*.cpp)
list(APPEND MATRIX_SOURCES "${PROTO_CPP_SOURCE}")
list(APPEND MATRIX_SOURCES ${TRACY_SOURCES})

# Exclude helper binaries and test harnesses that define their own entry point.
list(FILTER MATRIX_SOURCES EXCLUDE REGEX "trampoline\\.cpp$")
list(FILTER MATRIX_SOURCES EXCLUDE REGEX "/MultiPlayer/bootServer\\.cpp$")
list(FILTER MATRIX_SOURCES EXCLUDE REGEX "/MultiPlayer/bot\\.cpp$")
if(NOT MATRIX_ENABLE_WEBGUI)
  list(FILTER MATRIX_SOURCES EXCLUDE REGEX "/MultiPlayer/WebGui\\.cpp$")
endif()
list(REMOVE_DUPLICATES MATRIX_SOURCES)

# --- Core object library -----------------------------------------------------
# Everything except main.cpp lives here so the test binaries can link the real
# code. OBJECT (not STATIC) on purpose: the linker never prunes object files
# out of an object library, so global constructors cannot silently vanish.
list(FILTER MATRIX_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(matrix_core OBJECT ${MATRIX_SOURCES})

if(TARGET wlroots_build)
  add_dependencies(matrix_core wlroots_build)
endif()
if(TARGET generate_protocol_headers)
  add_dependencies(matrix_core generate_protocol_headers)
endif()
add_dependencies(matrix_core generate_protos)

set_target_properties(matrix_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(matrix_core PUBLIC
  include
  "${PROTO_CPP_OUTPUT_DIR}"
  include/imgui
  tracy/public
  /usr/local/include
  ${WLROOTS_LOCAL_INCLUDE_DIRS}
  ${WLROOTS_PRIVATE_INCLUDE_DIR}
  ${PROTOBUF_INCLUDE_DIRS}
  ${WLROOTS_PROTOCOL_DIR}
  ${MATRIX_PROTOCOL_GEN_DIR}
)

target_compile_definitions(matrix_core PUBLIC
  WLR_USE_UNSTABLE
)
if(MATRIX_ENABLE_WEBGUI)
  target_compile_definitions(matrix_core PUBLIC MATRIX_ENABLE_WEBGUI=1)
endif()

# Match the makefile's Arch/Void fmt handling.
if(EXISTS "/etc/os-release")
  file(READ "/etc/os-release" OS_RELEASE_CONTENT)
  string(FIND "${OS_RELEASE_CONTENT}" "void" OS_RELEASE_HAS_VOID)
  if(NOT OS_RELEASE_HAS_VOID EQUAL -1)
    target_compile_definitions(matrix_core PUBLIC SPDLOG_FMT_EXTERNAL)
  endif()
endif()

target_compile_options(matrix_core PRIVATE ${MATRIX_OPT_FLAGS})
matrix_apply_sanitizers(matrix_core)
if(MATRIX_WARNINGS)
  target_compile_options(matrix_core PRIVATE -Wall -Wextra)
endif()

# -march=native produces a binary that crashes with SIGILL on any CPU that is
# not identical to the build machine, which makes packages unusable. Keep it
# as an opt-in for local performance builds; -funroll-loops is portable.
set(MATRIX_PERF_OPTIONS "-funroll-loops")
if(MATRIX_NATIVE_ARCH)
  list(APPEND MATRIX_PERF_OPTIONS "-march=native")
endif()

# Apply loader flags only to the loader-style C sources.
set_source_files_properties(
  src/enkimi.c
  src/miniz.c
  PROPERTIES COMPILE_OPTIONS "${MATRIX_PERF_OPTIONS}"
)
set_source_files_properties(
  src/imgui/imgui_impl_opengl3.cpp
  PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_ES2
)

# ASan mis-compiles protobuf's generated translation unit with this toolchain:
# virtual calls from libprotobuf jump to a bogus address (verified with both GCC
# and Clang; the same code is fine without ASan). Keep only the generated file
# uninstrumented - the hand-written code is still fully checked.
if(MATRIX_SANITIZE)
  set_source_files_properties("${PROTO_CPP_SOURCE}"
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=${MATRIX_SANITIZE}")
endif()

target_link_directories(matrix_core PUBLIC
  ${WLROOTS_LOCAL_LIBRARY_DIRS}
  ${CMAKE_SOURCE_DIR}/lib
)

# RPATH into the submodule build tree is only meaningful when wlroots was built
# locally; a system wlroots lives in a standard linker path.
set(MATRIX_WLROOTS_RPATH_OPTIONS "")
if(NOT WLROOTS_SYSTEM_AVAILABLE)
  list(APPEND MATRIX_WLROOTS_RPATH_OPTIONS
    "-Wl,-rpath,${WLROOTS_BUILD_DIR}"
    "-Wl,-rpath,${WLROOTS_BUILD_DIR}/subprojects/wayland/src"
  )
endif()
target_link_options(matrix_core PUBLIC
  ${MATRIX_WLROOTS_RPATH_OPTIONS}
  "-Wl,--as-needed"
)

target_link_libraries(matrix_core PUBLIC
  ${WLROOTS_LOCAL_LIBRARIES}
  ${PROTOBUF_LIBRARIES}
  zmq
  xcb
  pixman-1
  spdlog
  fmt
  pthread
  assimp
  sqlite3
  EGL
  GLESv2
)
if(MATRIX_ENABLE_WEBGUI)
  target_link_libraries(matrix_core PUBLIC Crow::Crow)
endif()

# --- Executable --------------------------------------------------------------
add_executable(matrix src/main.cpp)

if(TARGET wlroots_build)
  add_dependencies(matrix wlroots_build)
endif()
if(TARGET generate_protocol_headers)
  add_dependencies(matrix generate_protocol_headers)
endif()
add_dependencies(matrix generate_protos)

target_compile_options(matrix PRIVATE ${MATRIX_OPT_FLAGS})
matrix_apply_sanitizers(matrix)
if(MATRIX_WARNINGS)
  target_compile_options(matrix PRIVATE -Wall -Wextra)
endif()

target_link_libraries(matrix PRIVATE matrix_core)

add_executable(bootServer
  src/MultiPlayer/bootServer.cpp
  src/MultiPlayer/Server.cpp
)

set_target_properties(bootServer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/deployTools"
)

target_include_directories(bootServer PRIVATE
  include
  /usr/local/include
)

target_compile_options(bootServer PRIVATE ${MATRIX_OPT_FLAGS})
matrix_apply_sanitizers(bootServer)
if(MATRIX_WARNINGS)
  target_compile_options(bootServer PRIVATE -Wall -Wextra)
endif()

target_link_libraries(bootServer PRIVATE
  pthread
  spdlog
  fmt
)

# --- API client examples ------------------------------------------------------
option(MATRIX_BUILD_EXAMPLES "Build API client examples (e.g. voxel_demo)" OFF)
if(MATRIX_BUILD_EXAMPLES)
  add_executable(voxel_demo
    client_libs/cpp/voxel_demo.cpp
    "${PROTO_CPP_SOURCE}"
  )
  target_include_directories(voxel_demo PRIVATE
    "${PROTO_CPP_OUTPUT_DIR}"
    include
  )
  target_link_libraries(voxel_demo PRIVATE
    ${PROTOBUF_LIBRARIES}
    zmq
    pthread
  )
  add_dependencies(voxel_demo generate_protos)
endif()

# --- Tests -------------------------------------------------------------------
if(MATRIX_BUILD_TESTS)
  include(CTest)
  enable_testing()

  find_package(GTest QUIET)

  if(NOT GTest_FOUND)
    message(WARNING
      "GTest not found; the test suites are disabled.\n"
      "  Install gtest (Arch: pacman -S gtest, Debian: libgtest-dev) or\n"
      "  configure with -DMATRIX_BUILD_TESTS=OFF to silence this warning.")
  else()
    # Unit tests: pure logic, no compositor. They link matrix_core, so they
    # exercise the real implementation rather than a parallel copy of it.
    add_executable(matrix_unit_tests
      tests/api.cpp
      tests/chunk.cpp
      tests/dynamicObject.cpp
      tests/voxelRaycast.cpp
      tests/voxelSpace.cpp
      tests/imguiKeymap.cpp
    )
    target_link_libraries(matrix_unit_tests PRIVATE
      matrix_core
      GTest::gtest
      GTest::gtest_main
    )
    matrix_apply_sanitizers(matrix_unit_tests)

    add_test(NAME matrix_unit_tests COMMAND matrix_unit_tests)
    set_tests_properties(matrix_unit_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      TIMEOUT 120
    )
    if(MATRIX_SANITIZE)
      # The system libprotobuf is uninstrumented, so mixing it with annotated
      # STL containers yields false container-overflow reports.
      set_tests_properties(matrix_unit_tests PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_container_overflow=0;UBSAN_OPTIONS=print_stacktrace=1")
    endif()

    # Integration test that spawns the compositor through ./launch but has no
    # main() of its own, so it uses gtest_main.
    add_executable(matrix_controls_spec_tests tests/controls_spec.cpp)
    target_link_libraries(matrix_controls_spec_tests PRIVATE
      matrix_core
      GTest::gtest
      GTest::gtest_main
    )
    matrix_apply_sanitizers(matrix_controls_spec_tests)
    target_compile_definitions(matrix_controls_spec_tests PRIVATE
      MATRIX_TEST_DEFAULT_COMPOSITOR_BIN="$<TARGET_FILE:matrix>"
    )
    add_dependencies(matrix_controls_spec_tests matrix)

    # Integration tests spawn the real compositor through ./launch. Both files
    # define their own main(), so they link only GTest::gtest.
    foreach(_spec wayland_basic wayland_menu_spec)
      add_executable(matrix_${_spec}_tests "tests/${_spec}.cpp")
      target_link_libraries(matrix_${_spec}_tests PRIVATE
        matrix_core
        GTest::gtest
      )
      # Point the harness at the freshly built binary instead of the old
      # hardcoded ./matrix-wlroots-debug (never a CMake target).
      target_compile_definitions(matrix_${_spec}_tests PRIVATE
        MATRIX_TEST_DEFAULT_COMPOSITOR_BIN="$<TARGET_FILE:matrix>"
      )
      matrix_apply_sanitizers(matrix_${_spec}_tests)
      add_dependencies(matrix_${_spec}_tests matrix)
    endforeach()

    # These need a live Wayland/DRM session and spawn child processes, so they
    # are opt-in for CTest. They are always built, so they can be run manually
    # (e.g. from a TTY or inside a nested compositor).
    if(MATRIX_ENABLE_INTEGRATION_TESTS)
      add_test(NAME matrix_controls_spec COMMAND matrix_controls_spec_tests)
      add_test(NAME matrix_wayland_basic COMMAND matrix_wayland_basic_tests)
      add_test(NAME matrix_wayland_menu_spec COMMAND matrix_wayland_menu_spec_tests)
      set_tests_properties(
        matrix_controls_spec matrix_wayland_basic matrix_wayland_menu_spec
        PROPERTIES
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          TIMEOUT 900
          LABELS integration
      )
    endif()
  endif()
endif()
