# SPDX-License-Identifier: Apache-2.0
#
# The userspace half, tied together so one ctest reaches every case.
#
# The kernel module stays on kbuild and is not built from here: it needs the running kernel's build
# tree, not this project's toolchain. Configure this to build the library, its testing surface and
# the suite; build the module with kernel/Makefile.

cmake_minimum_required(VERSION 3.16)

# One line at the root names the filesystem, and the project, every target name,
# install path and mount default below are read off it.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(fsname)

project(${FS_NAME}
    VERSION 0.1.0
    DESCRIPTION "The userspace library of this filesystem, and its test suite"
    LANGUAGES CXX)

# Release by default: an unspecified build type yields -O0.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The suite reaches states the library refuses to produce, so configuring from here turns that
# surface on. A standalone libroof configure leaves it off and installs the same archive.
if(FS_PUBLISH_NAMESPACE)
    set(FS_TESTING OFF CACHE BOOL "Build the testing surface libroof does not install")
else()
    set(FS_TESTING ON CACHE BOOL "Build the testing surface libroof does not install")
endif()

enable_testing()

add_subdirectory(libroof)

# The suite calls the library by its neutral namespace, so a published build has nothing for it
# to link against.
if(NOT FS_PUBLISH_NAMESPACE)
    add_subdirectory(tests)
endif()
