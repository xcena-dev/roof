// SPDX-License-Identifier: Apache-2.0
//
// file.cpp -- the syscalls behind the verbs, and the records the perm ioctls want.

#include "fs/file.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "fs/errors.hpp"
#include "uapi.h"

namespace fsuser
{

// The ioctl records under this tree's C++ names. The uapi header keeps the kernel's C spelling.
using FsCacheReq = struct fs_cache_req;
using FsPermReq = struct fs_perm_req;

// The header spells these for a caller without the kernel's, so they are held to the kernel's here.
static_assert(Permission::Read.bits == FS_PERM_READ);
static_assert(Permission::Write.bits == FS_PERM_WRITE);
static_assert(Permission::Delete.bits == FS_PERM_DELETE);
static_assert(Permission::Admin.bits == FS_PERM_ADMIN);
static_assert(Permission::Ioctl.bits == FS_PERM_IOCTL);
static_assert(Permission::Grant.bits == FS_PERM_GRANT);
static_assert(Permission::All.bits == FS_PERM_ALL);
static_assert(AnyId == FS_PERM_ANY_ID);
static_assert(MaxDelegations == FS_DELEG_MAX);
static_assert(MaxLabelBytes == FS_LABEL_LEN - 1);
static_assert(static_cast<std::uint32_t>(CachePolicy::Writeback) == FS_CACHE_WRITEBACK);
static_assert(static_cast<std::uint32_t>(CachePolicy::Uncached) == FS_CACHE_UNCACHED);

namespace
{

// @command is the _IOC value, which is 32 bits wide; ioctl(2) widens it at the call.
void sendPermRecord(std::int32_t descriptor, std::uint32_t command, FsPermReq& sent, std::string_view named)
{
    if (::ioctl(descriptor, command, &sent) != 0)
    {
        throw FsCodedError{std::string{named}, lastSystemError()};
    }
}

// The labels go NUL-padded into the kernel's fixed arrays. One too long to fit goes with no terminator,
// which the kernel refuses rather than reading a cut label as another role.
FsPermReq makeRoleRecord(std::string_view group, std::string_view role, Permission perms, std::uint32_t node)
{
    FsPermReq sent{};
    sent.kind = FS_PERM_KIND_ROLE;
    sent.perms = perms.bits;
    sent.node_id = node;
    static_cast<void>(group.copy(sent.group, sizeof(sent.group)));
    static_cast<void>(role.copy(sent.role, sizeof(sent.role)));
    return sent;
}

}  // namespace

// ── Mapping ─────────────────────────────────────────────────────────────

Mapping::Mapping(void* mapped, std::uint64_t bytes) noexcept
    : address_{mapped},
      bytes_{bytes}
{
}

Mapping::Mapping(Mapping&& other) noexcept
    : address_{std::exchange(other.address_, nullptr)},
      bytes_{std::exchange(other.bytes_, 0)}
{
}

Mapping& Mapping::operator=(Mapping&& other) noexcept
{
    if (this != &other)
    {
        unmap();
        address_ = std::exchange(other.address_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

Mapping::~Mapping()
{
    unmap();
}

void Mapping::unmap() noexcept
{
    if (address_ != nullptr)
    {
        ::munmap(address_, static_cast<std::size_t>(bytes_));
        address_ = nullptr;
        bytes_ = 0;
    }
}

// ── File ────────────────────────────────────────────────────────────────

File::File(std::int32_t taken) noexcept
    : held_{taken}
{
}

File::File(File&& other) noexcept
    : held_{std::exchange(other.held_, -1)}
{
}

File& File::operator=(File&& other) noexcept
{
    if (this != &other)
    {
        close();
        held_ = std::exchange(other.held_, -1);
    }
    return *this;
}

File::~File()
{
    close();
}

void File::close() noexcept
{
    if (held_ >= 0)
    {
        ::close(held_);
        held_ = -1;
    }
}

File File::open(std::string_view path, std::int32_t flags, ::mode_t mode)
{
    const std::string named{path};
    const std::int32_t taken = ::open(named.c_str(), flags | O_CLOEXEC, mode);

    if (taken < 0)
    {
        throw FsCodedError{"open " + named, lastSystemError()};
    }
    return File{taken};
}

void File::requireOpen(std::string_view verb) const
{
    if (held_ < 0)
    {
        throw FsError{std::string{verb} + ": the file holds nothing"};
    }
}

void File::resize(std::uint64_t bytes)
{
    requireOpen("resize");

    if (::ftruncate(held_, static_cast<::off_t>(bytes)) != 0)
    {
        throw FsCodedError{"ftruncate", lastSystemError()};
    }
}

Mapping File::map(std::uint64_t bytes, std::int32_t protection)
{
    requireOpen("map");

    // The permission question, asked before mmap(2) rather than inside it: the kernel holds this
    // process's mmap_lock while it runs mmap, and a daemon waited on there would stall every fault
    // of every other thread. Asked here, the wait holds nothing and mmap finds the row written.
    auto asked = Permission::Read;
    if ((protection & PROT_WRITE) != 0)
    {
        asked |= Permission::Write;
    }
    sendPermRequest(FS_IOC_PERM_ASK, 0, 0, asked, "perm_ask");

    auto* const mapped = ::mmap(nullptr, static_cast<std::size_t>(bytes),
                                protection, MAP_SHARED, held_, 0);

    if (mapped == MAP_FAILED)
    {
        throw FsCodedError{"mmap", lastSystemError()};
    }
    return Mapping{mapped, bytes};
}

void File::setCachePolicy(CachePolicy policy)
{
    requireOpen("cache_set");

    FsCacheReq sent{};
    sent.policy = static_cast<std::uint32_t>(policy);

    if (::ioctl(held_, FS_IOC_CACHE_SET, &sent) != 0)
    {
        throw FsCodedError{"cache_set", lastSystemError()};
    }
}

CachePolicy File::readCachePolicy()
{
    requireOpen("cache_get");

    FsCacheReq taken{};

    if (::ioctl(held_, FS_IOC_CACHE_GET, &taken) != 0)
    {
        throw FsCodedError{"cache_get", lastSystemError()};
    }

    return static_cast<CachePolicy>(taken.policy);
}

void File::setDefaultPermission(Permission perms)
{
    sendPermRequest(FS_IOC_PERM_SET_DEFAULT, 0, 0, perms, "perm_set_default");
}

void File::grantPermission(std::uint32_t uid, std::uint32_t gid, Permission perms)
{
    sendPermRequest(FS_IOC_PERM_GRANT, uid, gid, perms, "perm_grant");
}

void File::revokePermission(std::uint32_t uid, std::uint32_t gid)
{
    sendPermRequest(FS_IOC_PERM_REVOKE, uid, gid, Permission::None, "perm_revoke");
}

void File::grantRole(std::string_view group, std::string_view role, Permission perms, std::uint32_t node)
{
    requireOpen("perm_grant_role");
    auto sent = makeRoleRecord(group, role, perms, node);
    sendPermRecord(held_, FS_IOC_PERM_GRANT, sent, "perm_grant_role");
}

void File::revokeRole(std::string_view group, std::string_view role, std::uint32_t node)
{
    requireOpen("perm_revoke_role");
    auto sent = makeRoleRecord(group, role, Permission::None, node);
    sendPermRecord(held_, FS_IOC_PERM_REVOKE, sent, "perm_revoke_role");
}

// The kernel does its own ADMIN check next to the write, so nothing here judges the request.
void File::sendPermRequest(std::uint32_t command, std::uint32_t uid, std::uint32_t gid,
                           Permission perms, std::string_view named)
{
    requireOpen(named);

    FsPermReq sent{};

    sent.uid = uid;
    sent.gid = gid;
    sent.perms = perms.bits;

    sendPermRecord(held_, command, sent, named);
}

// ── free verbs ──────────────────────────────────────────────────────────

void unlinkFile(std::string_view path)
{
    const std::string named{path};

    if (::unlink(named.c_str()) != 0)
    {
        throw FsCodedError{"unlink " + named, lastSystemError()};
    }
}

}  // namespace fsuser
