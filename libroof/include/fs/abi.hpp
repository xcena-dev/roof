// SPDX-License-Identifier: Apache-2.0
//
// fs/abi.hpp -- what a program needs to load the shared library with dlopen and call it.
//
// The library exports one C function, which hands back a table of verbs for an interface version.
// A caller names nothing of the library but this header. Each verb works on a descriptor the caller
// owns and never closes, returns 0 or a negative errno, and never throws. Opening, resizing,
// mapping and unlinking are plain syscalls and are not verbs.

#pragma once

#include <cstdint>
#include <string_view>

#include "fs/file.hpp"
#include "fs/soname.hpp"  // IWYU pragma: export

// The exported function's name and the string dlsym is given for it, both built from the bare word
// that the publishing pass rewrites. Nothing else here is a macro.
#define FSUSER_C_NAME(verb) fsuser##_##verb
#define FSUSER_STRINGIZE(text) #text
#define FSUSER_STRING(text) FSUSER_STRINGIZE(text)
#define FSUSER_WORD fsuser
#define FSUSER_C_API_SYMBOL FSUSER_STRING(FSUSER_WORD) "_c_api_get"

namespace fsuser
{

// Moves when a verb changes meaning or signature. A verb appended to the table does not move it:
// the table opens with its own size, so a loader reads the prefix it knows.
inline constexpr std::uint32_t AbiVersion = 1;

struct CApi_t
{
    // sizeof this struct as the library was built, and the version it answers for.
    std::uint32_t size;
    std::uint32_t abiVersion;

    // Accepted only while the file has no extent. attr.pieces must be 0.
    std::int32_t (*setRegionAttr)(std::int32_t descriptor, const RegionAttr_t& attr) noexcept;

    // How the region landed, or what setRegionAttr asked for while it has no extent.
    std::int32_t (*readRegionAttr)(std::int32_t descriptor, RegionAttr_t* attr) noexcept;

    // What the region grants to everyone without a row of their own.
    std::int32_t (*setDefaultPermission)(std::int32_t descriptor, Permission perms) noexcept;

    // A delegation row for an account, and that row taken off. AnyId in one id leaves the other to decide.
    std::int32_t (*grantPermission)(std::int32_t descriptor, std::uint32_t uid, std::uint32_t gid,
                                    Permission perms) noexcept;
    std::int32_t (*revokePermission)(std::int32_t descriptor, std::uint32_t uid, std::uint32_t gid) noexcept;

    // A delegation row for a workload by its group and role, on @node or on every node with AnyNode.
    // A label past MaxLabelBytes answers -EINVAL.
    std::int32_t (*grantRole)(std::int32_t descriptor, std::string_view group, std::string_view role,
                              Permission perms, std::uint32_t node) noexcept;
    std::int32_t (*revokeRole)(std::int32_t descriptor, std::string_view group, std::string_view role,
                               std::uint32_t node) noexcept;

    // The permission question mapping asks, put before mmap(2) so a wait on the daemon holds no lock.
    std::int32_t (*askPermission)(std::int32_t descriptor, Permission perms) noexcept;
};

using CApiGetFn = const CApi_t* (*)(std::uint32_t abiVersion);

}  // namespace fsuser

// The table for @abiVersion, or null when this library does not serve that version.
extern "C" const fsuser::CApi_t* FSUSER_C_NAME(c_api_get)(std::uint32_t abiVersion);
