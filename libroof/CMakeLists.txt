# SPDX-License-Identifier: Apache-2.0
#
# libroof -- the library an application links to reach this filesystem's verbs.
#
# Configures from the top-level project or on its own, so a consumer that wants the library alone
# needs nothing else here. It builds apart from the kernel module and borrows one header from it.

cmake_minimum_required(VERSION 3.16)
project(libroof
    VERSION 0.1.0
    DESCRIPTION "File verbs on a mount, with descriptors that close themselves"
    LANGUAGES CXX)

include(GNUInstallDirs)

# Configuring this directory on its own still has to name the filesystem the same way the
# top-level configure does.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(fsname)

# Release by default: an unspecified build type yields -O0.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Publishing the namespace ────────────────────────────────────────────
set(FS_LIB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The directory an #include names. Neutral here, the filesystem's own in what ships.
set(FS_HEADER_DIR fs)
set(FS_LIB_SOURCES src/file.cpp)
set(FS_TESTING_SOURCES src/testing.cpp)
set(FS_EXAMPLE_SOURCES examples/overlap.cpp)

if(FS_PUBLISH_NAMESPACE)
    set(FS_LIB_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/published/include")
    set(FS_HEADER_DIR ${LIB_NAME})
    set(_fs_published "")
    file(GLOB_RECURSE _fs_originals RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    foreach(_fs_original IN LISTS _fs_originals)
        string(REPLACE "include/fs/" "include/${LIB_NAME}/" _fs_at "${_fs_original}")
        set(_fs_out "${CMAKE_CURRENT_BINARY_DIR}/published/${_fs_at}")
        execute_process(COMMAND "${FS_RENDER}" --namespace
                                "${CMAKE_CURRENT_SOURCE_DIR}/${_fs_original}" "${_fs_out}"
                        RESULT_VARIABLE _fs_done)
        if(NOT _fs_done EQUAL 0)
            message(FATAL_ERROR "publishing ${_fs_original} failed")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/${_fs_original}")
    endforeach()
    set(FS_LIB_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/published/src/file.cpp")
    set(FS_TESTING_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/published/src/testing.cpp")
    set(FS_EXAMPLE_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/published/examples/overlap.cpp")
    unset(_fs_originals)
    unset(_fs_original)
    unset(_fs_out)
    unset(_fs_at)
    unset(_fs_done)
    unset(_fs_published)
endif()

# ── The library ─────────────────────────────────────────────────────────
# Static, and nothing links through it: an application reaches the filesystem by syscall alone.
add_library(fs STATIC ${FS_LIB_SOURCES})

target_include_directories(fs
    PUBLIC
        $<BUILD_INTERFACE:${FS_LIB_INCLUDE}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)

set_target_properties(fs PROPERTIES OUTPUT_NAME "${LIB_NAME}")

target_compile_options(fs PRIVATE
    -Wall -Wextra -Wconversion -Wsign-conversion)

# ── Testing surface ─────────────────────────────────────────────────────
# A separate archive, so the installed library is the same one whether this is on or off. Putting
# these sources into fs instead would make a build flag decide what a host ships.
option(FS_TESTING "Build the testing surface libroof does not install" OFF)

if(FS_TESTING)
    add_library(fs-testing STATIC ${FS_TESTING_SOURCES})

    target_link_libraries(fs-testing PUBLIC fs)

    target_compile_options(fs-testing PRIVATE
        -Wall -Wextra -Wconversion -Wsign-conversion)
endif()

# ── Example ─────────────────────────────────────────────────────────────
# The case the library exists to close, which needs two mounts of one device to show at all. Built
# with the library and not installed: it is how to check a mount by hand, not something a host needs
# on PATH. The suite registers its default mode as a test.
add_executable(fs-overlap ${FS_EXAMPLE_SOURCES})
# The program prints its own name in usage and in every error it reports.
target_compile_definitions(fs-overlap PRIVATE FS_PROGRAM_NAME="${LIB_NAME}-overlap")
set_target_properties(fs-overlap PROPERTIES OUTPUT_NAME "${LIB_NAME}-overlap")
target_link_libraries(fs-overlap PRIVATE fs)
# The permission mask the perm verbs take is named in the kernel's uapi header.
target_include_directories(fs-overlap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)
target_compile_options(fs-overlap PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

# ── Install ─────────────────────────────────────────────────────────────
# testing.hpp is excluded rather than kept elsewhere: the directory is copied whole, so a header
# dropped beside the others would install no matter what FS_TESTING says.
# The headers land in a directory the filesystem names, so an application writes that name in
# front of the slash and two of these on one host do not collide.
install(TARGETS fs ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${FS_LIB_INCLUDE}/${FS_HEADER_DIR} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "testing.hpp" EXCLUDE)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libroof.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/lib${LIB_NAME}.pc
    @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib${LIB_NAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
