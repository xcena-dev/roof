// SPDX-License-Identifier: Apache-2.0
//
// fs/file.hpp -- one open file on this filesystem, and the mapping it hands out.
//
// Mounting is not here. The mount helper owns the stages a bring-up needs, and by the time an
// application links this library the filesystem is mounted.
//
// Exclusion is not here either. Every path that writes shared metadata takes the cross-node lock
// inside the kernel, on this node's mount, before it reads the state it is about to change. So the
// verbs below are the plain syscalls, and calling them through this library changes nothing about
// what the filesystem serialises. What it does spend for the caller is the descriptor and the
// mapping, and it sends the permission ioctls without making the caller name the request record.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string_view>

namespace fsuser
{

// The granule a region is placed in. A resize rounds up to it, and a mapping covers whole ones.
inline constexpr std::uint64_t DaxAlignment = 2ULL * 1024ULL * 1024ULL;

// What a permission verb grants or requires, as the kernel spells the bits. A mask type rather than
// an enum class, so a single bit and a combination are one type and or'ing stays inside it.
struct Permission
{
    std::uint32_t bits{0};

    static const Permission None;
    static const Permission Read;
    static const Permission Write;
    static const Permission Delete;
    static const Permission Admin;
    static const Permission Ioctl;
    static const Permission Grant;
    static const Permission All;

    [[nodiscard]] constexpr Permission operator|(Permission other) const noexcept
    {
        return Permission{bits | other.bits};
    }
    [[nodiscard]] constexpr Permission operator&(Permission other) const noexcept
    {
        return Permission{bits & other.bits};
    }
    constexpr Permission& operator|=(Permission other) noexcept
    {
        bits |= other.bits;
        return *this;
    }
    [[nodiscard]] constexpr bool operator==(Permission other) const noexcept
    {
        return bits == other.bits;
    }
    [[nodiscard]] constexpr bool operator!=(Permission other) const noexcept
    {
        return bits != other.bits;
    }
};

inline constexpr Permission Permission::None{0};
inline constexpr Permission Permission::Read{0x0001};
inline constexpr Permission Permission::Write{0x0002};
inline constexpr Permission Permission::Delete{0x0004};
inline constexpr Permission Permission::Admin{0x0008};
inline constexpr Permission Permission::Ioctl{0x0010};
inline constexpr Permission Permission::Grant{0x0020};
inline constexpr Permission Permission::All{0x003F};

// In one id of a grant or a revoke, leaves the other id deciding. Not zero, which is root.
inline constexpr std::uint32_t AnyId = ~0U;

// How many delegation rows one region holds. A grant past the last one answers ENOSPC.
inline constexpr std::uint32_t MaxDelegations = 29;

// How a region's bytes are cached. Uncached has no struct pages behind it, so nothing pins it for
// DMA, and in exchange a peer reads a write without the writer flushing.
enum class CachePolicy : std::uint32_t
{
    Writeback = 0,
    Uncached = 1,
};

class File;

// A mapping that unmaps itself, so no path out of a caller leaks one.
class Mapping
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept;
    ~Mapping();

    // ── operator= ──────────────────────────────────────────────────
    Mapping& operator=(const Mapping&) = delete;
    Mapping& operator=(Mapping&& other) noexcept;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] void* get() const noexcept
    {
        return address_;
    }
    [[nodiscard]] std::uint64_t getSize() const noexcept
    {
        return bytes_;
    }
    [[nodiscard]] bool isMapped() const noexcept
    {
        return address_ != nullptr;
    }

private:
    friend class File;
    Mapping(void* mapped, std::uint64_t bytes) noexcept;
    void unmap() noexcept;

private:
    void* address_{nullptr};
    std::uint64_t bytes_{0};
};

// One open file. It holds the descriptor and closes it, and everything it does is a syscall on that
// descriptor, so a File is as movable and as independent as the descriptor underneath it.
class File
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    File() = default;
    // Adopts @taken and closes it on the way out. A negative value is the failed-open one, which
    // every verb below then refuses rather than passing to the kernel.
    explicit File(std::int32_t taken) noexcept;
    File(const File&) = delete;
    File(File&& other) noexcept;
    ~File();

    // ── operator= ──────────────────────────────────────────────────
    File& operator=(const File&) = delete;
    File& operator=(File&& other) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // @path is a path under a mount of this filesystem. O_CLOEXEC rides along whether the caller
    // asked or not: the module refuses to map a descriptor an exec could have carried over.
    [[nodiscard]] static File open(std::string_view path, std::int32_t flags, ::mode_t mode = 0);

    // ── public methods ─────────────────────────────────────────────
    // Places this file's physical extent. The kernel scans every placed extent for a gap and
    // commits into it, holding the cross-node lock across both halves.
    void resize(std::uint64_t bytes);

    // The page tables are this node's own, and the first map that needs one has the kernel write a
    // delegation into the shared record under the lock.
    [[nodiscard]] Mapping map(std::uint64_t bytes, std::int32_t protection);

    // Which pool the placement is to take, and so how every later mapping of this file is cached.
    // Only a file with no extent yet accepts one, since resize() is what fixes it.
    void setCachePolicy(CachePolicy policy);

    // Where the region landed, or what setCachePolicy asked for while it has no extent.
    [[nodiscard]] CachePolicy readCachePolicy();

    // Replaces what this region grants to everyone with no delegation of their own. The kernel
    // checks ADMIN next to the write.
    void setDefaultPermission(Permission perms);

    // Writes a delegation for an account on this node. Either id alone is enough, and AnyId in
    // one leaves the other to decide.
    void grantPermission(std::uint32_t uid, std::uint32_t gid, Permission perms);

    // Takes that delegation back off. A row naming the caller needs no permission. Any other row
    // needs ADMIN, and a row belongs to the node that wrote it.
    void revokePermission(std::uint32_t uid, std::uint32_t gid);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] std::int32_t get() const noexcept
    {
        return held_;
    }
    [[nodiscard]] bool isOpen() const noexcept
    {
        return held_ >= 0;
    }

private:
    void close() noexcept;

    // Throws FsError naming @verb when this File holds no descriptor. Every verb below asks first,
    // so none of them hands -1 to the kernel.
    void requireOpen(std::string_view verb) const;

    // Every perm ioctl differs only in the command and which fields the kernel reads, so the
    // fields travel loose rather than as a record the caller would need the uapi header to name.
    void sendPermRequest(std::uint32_t command, std::uint32_t uid, std::uint32_t gid,
                         Permission perms, std::string_view named);

private:
    std::int32_t held_{-1};
};

// Removes @path. Only the node that created a file may remove it, so a peer's file answers EPERM.
void unlinkFile(std::string_view path);

}  // namespace fsuser
