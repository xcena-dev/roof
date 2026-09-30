# SPDX-License-Identifier: Apache-2.0
#
# tools -- the userspace programs that ship with the filesystem, built apart from the module.
#
# Standalone: this tree has no top-level CMake project, so each userspace piece configures on its
# own.
#
#   cmake -S . -B build
#   cmake --build build
#   sudo cmake --install build

cmake_minimum_required(VERSION 3.16)
project(fs-tools
    VERSION 0.1.0
    DESCRIPTION "mount and umount helpers for this filesystem"
    LANGUAGES CXX)

include(GNUInstallDirs)

# This project configures on its own, so it reads the tree's one name for itself.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(fsname)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# mount(8) and umount(8) look for a helper named after the filesystem type in /sbin alone, so this
# one destination is not derived from CMAKE_INSTALL_PREFIX.
set(FS_HELPER_DIR /sbin CACHE PATH "Where mount(8) looks for a type helper")

# ── the mount helper ────────────────────────────────────────────────────
# mount(8) picks the helper by the filesystem type it was given, so the name is the contract. It
# formats the lock region and starts that node's helper by running the tools those need, so it links
# nothing of cme itself.
add_executable(mount-helper mount_helper.cpp)
set_target_properties(mount-helper PROPERTIES OUTPUT_NAME "mount.${FS_NAME}")
# Every message it prints opens with that same name.
target_compile_definitions(mount-helper PRIVATE FS_PROGRAM_NAME="mount.${FS_NAME}")
target_include_directories(mount-helper PRIVATE ${FS_GENERATED_INCLUDE})

# ── the umount helper ───────────────────────────────────────────────────
# Stopping this node's helper and calling umount(2), which needs nothing of cme either.
add_executable(umount-helper umount_helper.cpp)
set_target_properties(umount-helper PROPERTIES OUTPUT_NAME "umount.${FS_NAME}")
target_compile_definitions(umount-helper PRIVATE FS_PROGRAM_NAME="umount.${FS_NAME}")
target_include_directories(umount-helper PRIVATE ${FS_GENERATED_INCLUDE})

foreach(helper mount-helper umount-helper)
    target_compile_options(${helper} PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
endforeach()

# ── tests ───────────────────────────────────────────────────────────────
# What daemon_command.hpp promises the mount helper: an argv reaches a child whole, never through
# a shell that would re-split it.
enable_testing()
add_executable(test-mount-helper-argv tests/test_mount_helper_argv.cpp)
target_compile_definitions(test-mount-helper-argv PRIVATE FS_PROGRAM_NAME="test-mount-helper-argv")
target_include_directories(test-mount-helper-argv PRIVATE ${FS_GENERATED_INCLUDE}
                                                            ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test-mount-helper-argv PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
add_test(NAME test-mount-helper-argv COMMAND test-mount-helper-argv)

# Which node id a mount table line names, which is what both helpers name the daemon after.
add_executable(test-mount-table tests/test_mount_table.cpp)
target_include_directories(test-mount-table PRIVATE ${FS_GENERATED_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test-mount-table PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
add_test(NAME test-mount-table COMMAND test-mount-table)

# ── Install ─────────────────────────────────────────────────────────────
# deploy/ is not installed from here. Its units name absolute paths and its script creates
# accounts, so it is run rather than laid down by a build.
install(TARGETS mount-helper umount-helper RUNTIME DESTINATION ${FS_HELPER_DIR})
