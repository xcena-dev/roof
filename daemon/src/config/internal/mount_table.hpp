// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config/internal/mount_table.hpp -- what the kernel's mount table tells this daemon about its node.
//
// The lock region is the one thing the config can leave out and still be complete, because the mount
// already carries it. The path is a parameter so a probe can hand over a table of its own.

#pragma once

#include <cstdint>
#include <string>

namespace fsdaemon::config::internal
{

// The lock region the mount of @nodeId carries, read from @mountsPath, or empty when no mount claims
// that id. Every node reaches one region through a mount point of its own, so the path is the
// mount's and not the id's.
[[nodiscard]] std::string findRegionForNode(std::uint32_t nodeId,
                                            const std::string& mountsPath = "/proc/self/mounts");

}  // namespace fsdaemon::config::internal
