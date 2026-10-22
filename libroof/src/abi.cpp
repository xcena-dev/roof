// SPDX-License-Identifier: Apache-2.0
//
// abi.cpp -- the C table of verbs, and the one place the region and permission ioctls are sent.

#include "fs/abi.hpp"

#include <sys/ioctl.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "fs/file.hpp"
#include "uapi.h"

namespace fsuser
{

// These fail when a type that crosses the table stops being plain data.
static_assert(sizeof(Permission) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<Permission>);
static_assert(std::is_standard_layout_v<RegionAttr_t>);

namespace
{

using FsPermReq = struct fs_perm_req;
using FsRegionAttrReq = struct fs_region_attr_req;

// The kernel's errno, negated, or 0. @command is the _IOC value, which is 32 bits wide.
[[nodiscard]] std::int32_t sendIoctl(std::int32_t descriptor, std::uint32_t command, void* record) noexcept
{
    return (::ioctl(descriptor, command, record) == 0) ? 0 : -errno;
}

[[nodiscard]] std::int32_t sendAccountRecord(std::int32_t descriptor, std::uint32_t command, std::uint32_t uid,
                                             std::uint32_t gid, Permission perms) noexcept
{
    FsPermReq sent{};
    sent.uid = uid;
    sent.gid = gid;
    sent.perms = perms.bits;
    return sendIoctl(descriptor, command, &sent);
}

// The labels go NUL-padded into the kernel's fixed arrays, so one past MaxLabelBytes has no room
// for its terminator and is refused here rather than read by the kernel as another role.
[[nodiscard]] std::optional<FsPermReq> makeRoleRecord(std::string_view group, std::string_view role, Permission perms,
                                                      std::uint32_t node) noexcept
{
    if (group.size() > MaxLabelBytes || role.size() > MaxLabelBytes)
    {
        return std::nullopt;
    }
    FsPermReq sent{};
    sent.kind = FS_PERM_KIND_ROLE;
    sent.perms = perms.bits;
    sent.node_id = node;
    static_cast<void>(group.copy(sent.group, sizeof(sent.group)));
    static_cast<void>(role.copy(sent.role, sizeof(sent.role)));
    return sent;
}

std::int32_t setRegionAttr(std::int32_t descriptor, const RegionAttr_t& attr) noexcept
{
    FsRegionAttrReq sent{};
    sent.cache = static_cast<std::uint32_t>(attr.cache);
    sent.placement = static_cast<std::uint32_t>(attr.placement);
    sent.pieces = attr.pieces;
    sent.device = attr.device;
    return sendIoctl(descriptor, FS_IOC_REGION_ATTR_SET, &sent);
}

std::int32_t readRegionAttr(std::int32_t descriptor, RegionAttr_t* attr) noexcept
{
    FsRegionAttrReq taken{};
    const std::int32_t result = sendIoctl(descriptor, FS_IOC_REGION_ATTR_GET, &taken);
    if (result == 0 && attr != nullptr)
    {
        *attr = RegionAttr_t{static_cast<CachePolicy>(taken.cache),
                             static_cast<Placement>(taken.placement),
                             taken.pieces, taken.device};
    }
    return result;
}

std::int32_t setDefaultPermission(std::int32_t descriptor, Permission perms) noexcept
{
    return sendAccountRecord(descriptor, FS_IOC_PERM_SET_DEFAULT, 0, 0, perms);
}

std::int32_t grantPermission(std::int32_t descriptor, std::uint32_t uid, std::uint32_t gid, Permission perms) noexcept
{
    return sendAccountRecord(descriptor, FS_IOC_PERM_GRANT, uid, gid, perms);
}

std::int32_t revokePermission(std::int32_t descriptor, std::uint32_t uid, std::uint32_t gid) noexcept
{
    return sendAccountRecord(descriptor, FS_IOC_PERM_REVOKE, uid, gid, Permission::None);
}

std::int32_t grantRole(std::int32_t descriptor, std::string_view group, std::string_view role, Permission perms,
                       std::uint32_t node) noexcept
{
    auto sent = makeRoleRecord(group, role, perms, node);
    return sent ? sendIoctl(descriptor, FS_IOC_PERM_GRANT, &*sent) : -EINVAL;
}

std::int32_t revokeRole(std::int32_t descriptor, std::string_view group, std::string_view role,
                        std::uint32_t node) noexcept
{
    auto sent = makeRoleRecord(group, role, Permission::None, node);
    return sent ? sendIoctl(descriptor, FS_IOC_PERM_REVOKE, &*sent) : -EINVAL;
}

std::int32_t askPermission(std::int32_t descriptor, Permission perms) noexcept
{
    return sendAccountRecord(descriptor, FS_IOC_PERM_ASK, 0, 0, perms);
}

const CApi_t Table{sizeof(CApi_t), AbiVersion, setRegionAttr, readRegionAttr, setDefaultPermission,
                   grantPermission, revokePermission, grantRole, revokeRole, askPermission};

}  // namespace

}  // namespace fsuser

extern "C" const fsuser::CApi_t* FSUSER_C_NAME(c_api_get)(std::uint32_t abiVersion)
{
    return (abiVersion == fsuser::AbiVersion) ? &fsuser::Table : nullptr;
}
