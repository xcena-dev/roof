// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// model.hpp -- the two records that cross this daemon's layers, and nothing else.
//
// serve builds the credentials off the wire, identity resolves them to an identity, and policy
// reads that identity. Each of those layers needs these two shapes and none of the interfaces
// around them, so the shapes live here and carry no behaviour: a layer that only passes a record on
// includes this and stays clear of whoever produced it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace fsdaemon
{

// The calling task's credentials, as a selector matches them.
//
// Every field but the group list is the kernel's word, carried in the upcall: the ids, the start
// time, the exe path and the exe inode. The kernel holds the caller for the whole upcall, so those
// cannot go stale under this daemon, and nothing re-reads them. The group list is the exception,
// because the upcall does not carry it and only /proc/<pid>/status has it. The start time is what
// says that pid still names the task the kernel read.
struct Creds_t
{
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    // CLOCK_BOOTTIME nanoseconds, of the thread group leader whose pid the upcall carries.
    std::uint64_t startBoottimeNs{0};
    std::string exePath;
    std::uint64_t exeInodeIno{0};
    std::uint32_t exeInodeDev{0};
    // A set, because every read of it is a membership test: does this task carry that gid.
    std::unordered_set<std::uint32_t> supplementaryGids;
};

// Who a task is, as the policy reads it: a group and a role, and the SPIFFE id they came from.
struct Identity_t
{
    std::string spiffeId;
    std::string group;
    std::string role;
    // The selector labels that matched, for the audit trail. They do not change what the policy
    // decides, so a known-identity listing leaves them off.
    std::vector<std::string> selectors;
};

}  // namespace fsdaemon
