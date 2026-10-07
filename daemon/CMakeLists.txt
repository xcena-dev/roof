# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# daemon (C++) -- the userspace helper that answers the module's upcalls.
#
# Built standalone from this directory: cmake -S daemon -B <build>. The kernel and cme trees are
# read-only; nothing here writes into them.

cmake_minimum_required(VERSION 3.20)
project(daemon_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ── The tree's names ────────────────────────────────────────────────────
# The repo root is one level up. The names are read from the same lines the kernel reads, so a
# rename there reaches this build too.
get_filename_component(FS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
foreach(_fs_key FS_NAME DAEMON_NAME)
    file(STRINGS "${FS_ROOT}/fsname" _fs_name_line REGEX "^${_fs_key}=")
    if(NOT _fs_name_line)
        message(FATAL_ERROR "daemon: ${FS_ROOT}/fsname carries no ${_fs_key}= line")
    endif()
    string(REGEX REPLACE "^${_fs_key}=" "" ${_fs_key} "${_fs_name_line}")
endforeach()
string(TOUPPER "${FS_NAME}" FS_UPPER)
unset(_fs_key)
unset(_fs_name_line)

# ── The uapi headers, rendered into the build tree ──────────────────────
# The kernel ships only the *.h.in templates; the rendered headers are generated. This renders the
# ones daemon reads into the build tree rather than into kernel/include, so the read-only tree stays
# untouched.
set(FS_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${FS_GEN}")
set(FS_RENDER "${FS_ROOT}/tools/render-fsname.sh")
foreach(_uapi daemon_uapi uapi name tools_uapi)
    execute_process(
        COMMAND "${FS_RENDER}" "${FS_ROOT}/kernel/include/${_uapi}.h.in" "${FS_GEN}/${_uapi}.h"
        RESULT_VARIABLE _render_rc
        OUTPUT_VARIABLE _render_out ERROR_VARIABLE _render_out)
    if(NOT _render_rc EQUAL 0)
        message(FATAL_ERROR "daemon: could not render ${_uapi}.h: ${_render_out}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FS_ROOT}/kernel/include/${_uapi}.h.in")
endforeach()
unset(_uapi)

# daemon's own name header, on the same one name.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/name.hpp.in"
    "${FS_GEN}/name.hpp"
    @ONLY)

# ── Warnings, as the tree holds every target to ─────────────────────────
add_library(daemon_warnings INTERFACE)
target_compile_options(daemon_warnings INTERFACE -Wall -Wextra -Werror)

# ── The include roots every target shares ───────────────────────────────
# src/ for the daemon's headers, the build gen/ for the rendered uapi and name headers.
add_library(daemon_includes INTERFACE)
target_include_directories(daemon_includes INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FS_GEN}")

# cme lives in its own tree. Only its header-only YAML subset (cme_common) is needed until turn
# comes in. The libcme link waits behind DAEMON_ENABLE_TURN, so the core does not need a cme build.
set(DAEMON_CME_DIR "$ENV{HOME}/cme" CACHE PATH "the cme source tree, for cme_common and libcme")
option(DAEMON_ENABLE_TURN "Link libcme for cross-node lock/unlock" OFF)

# ── The daemon's own code ───────────────────────────────────────────────
# One archive the probes and main link. Sources are listed as each lands, so a file that stops
# building is a named failure rather than one a glob quietly drops.
add_library(daemon_core STATIC
    src/posix/notify.cpp
    src/serve/channel.cpp
    src/serve/handler.cpp
    src/serve/loop.cpp
    src/serve/worker_pool.cpp
    src/identity/base.cpp
    src/identity/internal/selector.cpp
    src/identity/internal/spiffe.cpp
    src/identity/local.cpp
    src/identity/spire.cpp)
target_link_libraries(daemon_core PUBLIC daemon_includes PRIVATE daemon_warnings)
find_package(Threads REQUIRED)
target_link_libraries(daemon_core PUBLIC Threads::Threads)

# The sha256 selector digests the caller's exe, so the core takes libcrypto.
find_package(OpenSSL REQUIRED)
target_link_libraries(daemon_core PRIVATE OpenSSL::Crypto)

# ── Config and audit ────────────────────────────────────────────────────
# config reads config.yaml with cme's header-only key-value reader, so it takes cme_common's include
# path (no link). audit rides along here; it needs neither cme nor a heavy dependency.
add_library(daemon_config STATIC
    src/config/config.cpp
    src/config/internal/mount_table.cpp)
target_include_directories(daemon_config PUBLIC "${DAEMON_CME_DIR}/common")
target_link_libraries(daemon_config PUBLIC daemon_includes PRIVATE daemon_warnings)

# ── The policy decision, shared by every backend ─────────────────────────
# The JSON reader and the input/output shape a decision is built and read with. Both backends read
# the same decision object, so this is always built and each backend links it. It has no external
# dependency, so an OPA-only or mock build needs neither cargo nor libcurl.
add_library(daemon_policy_core STATIC
    src/policy/base.cpp)
target_link_libraries(daemon_policy_core PUBLIC daemon_includes PRIVATE daemon_warnings)

# ── The in-process rego backend (regorus C FFI) ──────────────────────────
# Its own library, so the probes that need no policy do not pull in the cargo-built archive. A Rust
# staticlib wraps regorus; cargo builds it, and this links it with the system libraries a Rust
# staticlib needs. Off falls back to a daemon with no policy engine (access denies fail-closed).
option(DAEMON_ENABLE_POLICY_REGORUS "Build the in-process rego policy backend via the regorus C FFI" ON)
if(DAEMON_ENABLE_POLICY_REGORUS)
    find_program(DAEMON_CARGO cargo REQUIRED HINTS "$ENV{HOME}/.cargo/bin")
    set(REGORUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/regorus_ffi")
    set(REGORUS_LIB "${REGORUS_DIR}/target/release/libdaemon_regorus_ffi.a")
    # Named so an edit to the Rust side reaches the archive. Without them CMake sees the archive
    # already there, skips cargo, and links yesterday's policy engine.
    # Cargo.lock is untracked (daemon/.gitignore), so a fresh checkout has none. Give the generator a
    # rule to produce it instead of naming it a prerequisite that must already exist.
    set(REGORUS_LOCK "${REGORUS_DIR}/Cargo.lock")
    add_custom_command(
        OUTPUT "${REGORUS_LOCK}"
        COMMAND "${DAEMON_CARGO}" generate-lockfile --manifest-path "${REGORUS_DIR}/Cargo.toml"
        DEPENDS "${REGORUS_DIR}/Cargo.toml"
        WORKING_DIRECTORY "${REGORUS_DIR}"
        COMMENT "daemon: generating the regorus C FFI lockfile (cargo)"
        VERBATIM)
    set(REGORUS_INPUTS
        "${REGORUS_DIR}/Cargo.toml"
        "${REGORUS_LOCK}"
        "${REGORUS_DIR}/build.rs"
        "${REGORUS_DIR}/cbindgen.toml"
        "${REGORUS_DIR}/src/lib.rs")
    add_custom_command(
        OUTPUT "${REGORUS_LIB}"
        COMMAND "${DAEMON_CARGO}" build --release --manifest-path "${REGORUS_DIR}/Cargo.toml"
        DEPENDS ${REGORUS_INPUTS}
        WORKING_DIRECTORY "${REGORUS_DIR}"
        COMMENT "daemon: building the regorus C FFI (cargo)"
        VERBATIM)
    add_custom_target(daemon_regorus_build DEPENDS "${REGORUS_LIB}")
    add_library(daemon_regorus_ffi STATIC IMPORTED GLOBAL)
    set_target_properties(daemon_regorus_ffi PROPERTIES IMPORTED_LOCATION "${REGORUS_LIB}")

    # The one target that names the C linkage. The crate's src/ is a private include dir, so nothing
    # above this library can reach the generated prototypes.
    add_library(daemon_regorus_binding STATIC "${REGORUS_DIR}/src/engine.cpp")
    add_dependencies(daemon_regorus_binding daemon_regorus_build)
    target_include_directories(daemon_regorus_binding
        PUBLIC "${REGORUS_DIR}/include"
        PRIVATE "${REGORUS_DIR}/src")
    target_link_libraries(daemon_regorus_binding
        PUBLIC daemon_regorus_ffi Threads::Threads ${CMAKE_DL_LIBS} m
        PRIVATE daemon_warnings)

    add_library(daemon_policy STATIC
        src/policy/local.cpp)
    target_link_libraries(daemon_policy
        PUBLIC daemon_policy_core daemon_regorus_binding
        PRIVATE daemon_warnings)
endif()

# ── The OPA REST backend ─────────────────────────────────────────────────
# The core builds the input and reads the decision over an injected transport, so it links no HTTP
# library and its mock test runs in every build. The curl transport is the only part that links
# libcurl, so it waits behind DAEMON_ENABLE_OPA. Off leaves a daemon that cannot reach an OPA server.
add_library(daemon_opa_core STATIC
    src/policy/opa.cpp)
target_link_libraries(daemon_opa_core PUBLIC daemon_policy_core PRIVATE daemon_warnings)

option(DAEMON_ENABLE_OPA "Build the libcurl HTTP transport for the OPA policy backend" OFF)
if(DAEMON_ENABLE_OPA)
    find_package(CURL REQUIRED)
    # Its own library, so a probe drives the real exchange without linking the registry and the
    # config that the registration beside it reads.
    add_library(daemon_opa_transport STATIC
        src/policy/opa_transport.cpp)
    target_link_libraries(daemon_opa_transport
        PUBLIC daemon_opa_core PRIVATE CURL::libcurl daemon_warnings)

    add_library(daemon_opa INTERFACE)
    target_link_libraries(daemon_opa INTERFACE daemon_opa_transport CURL::libcurl)
endif()

# ── The SPIRE gRPC identity backend ─────────────────────────────────────
# The provider core is in daemon_core and needs no gRPC, so its mock test builds everywhere. The
# fetcher to a SPIRE agent is behind DAEMON_ENABLE_SPIRE, since it links gRPC and protobuf. The
# SPIFFE .proto are generated with protoc into the build tree, kept in their own library because the
# generated code does not pass -Wall -Wextra -Werror, which only the daemon's own code is held to.
option(DAEMON_ENABLE_SPIRE "Build the gRPC SPIRE identity backend" OFF)
if(DAEMON_ENABLE_SPIRE)
    find_package(Protobuf REQUIRED)
    find_program(DAEMON_PROTOC protoc REQUIRED)
    find_program(DAEMON_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)

    set(SPIRE_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/identity/spire_proto")
    set(SPIRE_GEN "${CMAKE_CURRENT_BINARY_DIR}/spire_gen")
    file(MAKE_DIRECTORY "${SPIRE_GEN}")

    # Every message file gets a protobuf stub. delegatedidentity is the only one with a service, so
    # only it gets the gRPC stub.
    set(SPIRE_MESSAGE_PROTOS spiffeid x509svid jwtsvid selector delegatedidentity)
    set(SPIRE_GEN_SRCS "")
    foreach(_proto ${SPIRE_MESSAGE_PROTOS})
        add_custom_command(
            OUTPUT "${SPIRE_GEN}/${_proto}.pb.cc" "${SPIRE_GEN}/${_proto}.pb.h"
            COMMAND "${DAEMON_PROTOC}" -I "${SPIRE_PROTO_DIR}"
                    --cpp_out "${SPIRE_GEN}" "${SPIRE_PROTO_DIR}/${_proto}.proto"
            DEPENDS "${SPIRE_PROTO_DIR}/${_proto}.proto"
            COMMENT "daemon: protoc ${_proto}.proto"
            VERBATIM)
        list(APPEND SPIRE_GEN_SRCS "${SPIRE_GEN}/${_proto}.pb.cc")
    endforeach()
    unset(_proto)
    add_custom_command(
        OUTPUT "${SPIRE_GEN}/delegatedidentity.grpc.pb.cc" "${SPIRE_GEN}/delegatedidentity.grpc.pb.h"
        COMMAND "${DAEMON_PROTOC}" -I "${SPIRE_PROTO_DIR}"
                --grpc_out "${SPIRE_GEN}"
                "--plugin=protoc-gen-grpc=${DAEMON_GRPC_CPP_PLUGIN}"
                "${SPIRE_PROTO_DIR}/delegatedidentity.proto"
        DEPENDS "${SPIRE_PROTO_DIR}/delegatedidentity.proto"
        COMMENT "daemon: protoc grpc delegatedidentity.proto"
        VERBATIM)
    list(APPEND SPIRE_GEN_SRCS "${SPIRE_GEN}/delegatedidentity.grpc.pb.cc")

    add_library(daemon_spire_proto STATIC ${SPIRE_GEN_SRCS})
    target_include_directories(daemon_spire_proto PUBLIC "${SPIRE_GEN}")
    target_link_libraries(daemon_spire_proto PUBLIC PkgConfig::GRPCPP protobuf::libprotobuf)

    # The fetcher compiles into the daemon beside the registration it serves, so this target carries
    # what that translation unit links and no source of its own.
    add_library(daemon_spire INTERFACE)
    target_link_libraries(daemon_spire INTERFACE daemon_core daemon_spire_proto)
endif()

# ── The turn backend (cross-node lock/unlock via cme) ───────────────────
# Its own library, linking the prebuilt libcme.a. cme is read-only, so this links what its build
# produced rather than building it here. Off leaves the daemon reporting no lock service.
if(DAEMON_ENABLE_TURN)
    set(DAEMON_CME_LIB "${DAEMON_CME_DIR}/build/libcme.a" CACHE FILEPATH "prebuilt libcme.a")
    if(NOT EXISTS "${DAEMON_CME_LIB}")
        message(FATAL_ERROR "daemon: no libcme.a at ${DAEMON_CME_LIB}; build cme or set DAEMON_CME_LIB")
    endif()
    add_library(daemon_cme STATIC IMPORTED GLOBAL)
    set_target_properties(daemon_cme PROPERTIES IMPORTED_LOCATION "${DAEMON_CME_LIB}")

    add_library(daemon_turn STATIC src/turn/cme_turn.cpp)
    target_include_directories(daemon_turn PUBLIC "${DAEMON_CME_DIR}/include")
    target_link_libraries(daemon_turn
        PUBLIC daemon_includes daemon_cme Threads::Threads
        PRIVATE daemon_warnings)
endif()

# ── The daemon binary ───────────────────────────────────────────────────
# Wires the local identity backend to the serve loop. It opens the real char device, so a test
# drives ServeLoop over a socketpair instead and never runs this.
# The registry a backend registers itself into. It names no backend, so it links none.
add_library(daemon_backends STATIC src/backends.cpp)
target_link_libraries(daemon_backends PUBLIC daemon_core daemon_config PRIVATE daemon_warnings)

# ── The daemon binary ───────────────────────────────────────────────────
# Each backend's registration compiles straight into the executable rather than reaching it through
# an archive, because a linker drops an archive member nothing names and a registration is exactly
# that. Which registrations compile is the whole of which backends this build carries.
add_executable(daemon
    src/main.cpp
    src/commands.cpp
    src/identity/local_backend.cpp)
target_link_libraries(daemon PRIVATE daemon_backends daemon_warnings)
if(DAEMON_ENABLE_POLICY_REGORUS)
    target_sources(daemon PRIVATE src/policy/local_backend.cpp)
    target_link_libraries(daemon PRIVATE daemon_policy)
endif()
if(DAEMON_ENABLE_OPA)
    target_sources(daemon PRIVATE src/policy/opa_backend.cpp)
    target_link_libraries(daemon PRIVATE daemon_opa)
endif()
if(DAEMON_ENABLE_SPIRE)
    target_sources(daemon PRIVATE src/identity/spire_backend.cpp)
    target_link_libraries(daemon PRIVATE daemon_spire)
endif()
if(DAEMON_ENABLE_TURN)
    target_sources(daemon PRIVATE src/turn/cme_backend.cpp)
    target_link_libraries(daemon PRIVATE daemon_turn)
endif()

enable_testing()
add_subdirectory(tests)
