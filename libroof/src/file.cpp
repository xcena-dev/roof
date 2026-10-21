// SPDX-License-Identifier: Apache-2.0
//
// file.cpp -- the syscalls behind the verbs. The ioctls are sent through the C table.

#include "fs/file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "fs/abi.hpp"
#include "fs/errors.hpp"
#include "uapi.h"

namespace fsuser
{

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
static_assert(static_cast<std::uint32_t>(Placement::Contiguous) == FS_PLACEMENT_CONTIGUOUS);
static_assert(static_cast<std::uint32_t>(Placement::Paged) == FS_PLACEMENT_PAGED);

namespace
{

// The table this library serves for its own interface version, which it always does.
const CApi_t& abiTable()
{
    return *FSUSER_C_NAME(c_api_get)(AbiVersion);
}

// A negative return is the entry point's -errno.
void requireDone(std::int32_t result, std::string_view named)
{
    if (result < 0)
    {
        throw FsCodedError{std::string{named}, std::error_code{-result, std::system_category()}};
    }
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
    requireDone(abiTable().askPermission(held_, asked), "perm_ask");

    auto* const mapped = ::mmap(nullptr, static_cast<std::size_t>(bytes),
                                protection, MAP_SHARED, held_, 0);

    if (mapped == MAP_FAILED)
    {
        throw FsCodedError{"mmap", lastSystemError()};
    }
    return Mapping{mapped, bytes};
}

void File::setRegionAttr(const RegionAttr_t& attr)
{
    requireOpen("region_attr_set");

    requireDone(abiTable().setRegionAttr(held_, attr), "region_attr_set");
}

RegionAttr_t File::readRegionAttr()
{
    requireOpen("region_attr_get");

    RegionAttr_t taken{};
    requireDone(abiTable().readRegionAttr(held_, &taken), "region_attr_get");

    return taken;
}

void File::setDefaultPermission(Permission perms)
{
    requireOpen("perm_set_default");
    requireDone(abiTable().setDefaultPermission(held_, perms), "perm_set_default");
}

void File::grantPermission(std::uint32_t uid, std::uint32_t gid, Permission perms)
{
    requireOpen("perm_grant");
    requireDone(abiTable().grantPermission(held_, uid, gid, perms), "perm_grant");
}

void File::revokePermission(std::uint32_t uid, std::uint32_t gid)
{
    requireOpen("perm_revoke");
    requireDone(abiTable().revokePermission(held_, uid, gid), "perm_revoke");
}

void File::grantRole(std::string_view group, std::string_view role, Permission perms, std::uint32_t node)
{
    requireOpen("perm_grant_role");
    requireDone(abiTable().grantRole(held_, group, role, perms, node), "perm_grant_role");
}

void File::revokeRole(std::string_view group, std::string_view role, std::uint32_t node)
{
    requireOpen("perm_revoke_role");
    requireDone(abiTable().revokeRole(held_, group, role, node), "perm_revoke_role");
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
