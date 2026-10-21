# SPDX-License-Identifier: Apache-2.0
#
# common -- the header-only helpers this tree shares with CME: the clock and its budgets, polled
# waits, atomics whose type fixes their memory order, and the YAML subset the config reader parses.
# They are copies of CME's common/common and are not edited here, so a change lands in CME first and
# is copied over whole. Every CMake project in this tree takes them by linking fs-common.

add_library(fs-common INTERFACE)
target_include_directories(fs-common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
