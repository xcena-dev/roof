// SPDX-License-Identifier: Apache-2.0
//
// test_lock_region_guard -- the lock region at a mount's root is the daemon's alone: no caller
// creates, unlinks, renames it, grants or revokes permission on it, or reaches a write mapping
// past its default.
//
// The narrowing itself is checked too, because it is what a mount leaves behind before any daemon
// arrives, on a device the same mount formatted as well as on one it joined.
//
//   test_lock_region_guard <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"
#include "name.h"
#include "tools_uapi.h"
#include "uapi.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// Lookup resolves this name to the lock inode before any create or rename op runs, so the three
// verbs below never reach a create, a real unlink, or a rename handler at all.
void checkCreateUnlinkRename(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("create, unlink and rename against the reserved name");

    report.checkRefusedWith(
        "O_CREAT|O_EXCL answers EEXIST because lookup already resolves the name",
        std::errc::file_exists,
        [&]
        {
            (void)fsuser::File::open(mount.pathTo(FS_LOCK_REGION_NAME),
                                     O_CREAT | O_EXCL | O_RDWR, 0600);
        });

    report.checkRefusedWith("unlink of the lock region answers EPERM",
                            std::errc::operation_not_permitted,
                            [&]
                            {
                                mount.unlink(FS_LOCK_REGION_NAME);
                            });

    const auto tmpName = caseName("lock_region_guard_tmp");
    auto tmpFile = fsuser::File::open(mount.pathTo(tmpName), O_CREAT | O_RDWR, 0600);

    errno = 0;
    const std::int32_t renamed = ::rename(mount.pathTo(tmpName).c_str(),
                                          mount.pathTo(FS_LOCK_REGION_NAME).c_str());
    report.checkErrno("rename onto the lock region answers EPERM",
                      renamed < 0 && errno == EPERM, errno);

    mount.unlink(tmpName);
}

// Both verbs guard on the region's type before they look at any row, so an account with no row at
// all still answers EPERM rather than the ENOENT a missing row would otherwise give.
void checkGrantRevoke(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("PERM_GRANT and PERM_REVOKE against the lock region");

    auto region = fsuser::File::open(mount.pathTo(FS_LOCK_REGION_NAME), O_RDONLY);
    const auto callerUid = static_cast<std::uint32_t>(::getuid());

    report.checkRefusedWith("PERM_GRANT on the lock region answers EPERM",
                            std::errc::operation_not_permitted,
                            [&]
                            {
                                region.grantPermission(callerUid, fsuser::AnyId,
                                                       fsuser::Permission::Read);
                            });

    report.checkRefusedWith("PERM_REVOKE on the lock region answers EPERM",
                            std::errc::operation_not_permitted,
                            [&]
                            {
                                region.revokePermission(callerUid, fsuser::AnyId);
                            });
}

// The Default column of the perm_info row for @ratEntry, or nullopt when the file or the row is
// not there. The rows read "<entry>\t0x<default>\t<bound>".
[[nodiscard]] std::optional<std::uint32_t> readRegionDefault(std::uint32_t ratEntry)
{
    std::ifstream source{fsuser::tests::PermInfoPath};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return std::nullopt;
    }
    while (std::getline(source, line))
    {
        const auto fields = fsuser::tests::splitOnTabs(line);
        if (fields.size() < 3 || fields[0] != std::to_string(ratEntry))
        {
            continue;
        }
        try
        {
            return static_cast<std::uint32_t>(std::stoul(fields[1], nullptr, 0));
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// The account row this node's mount wrote into the lock region, or nullopt when the rows cannot be
// read. deleg_info takes the region to dump as a write, so only root reaches it.
[[nodiscard]] std::optional<std::uint32_t> readLockAccountPerms(std::uint32_t nodeId)
{
    const std::string path = "/sys/fs/" FS_NAME_STR "/deleg_info";
    {
        std::ofstream select{path};
        if (!select)
        {
            return std::nullopt;
        }
        select << FS_LOCK_REGION_RAT_ID << "\n";
        if (!select)
        {
            return std::nullopt;
        }
    }

    std::ifstream source{path};
    std::string line;
    while (std::getline(source, line))
    {
        // pid 0 is what makes a row an account row rather than one bound to a process.
        if (line.find("deleg[") == std::string::npos || line.find(" account ") == std::string::npos)
        {
            continue;
        }
        const auto rowNode = fsuser::tests::readTagged(line, "node=");
        const auto rowPid = fsuser::tests::readTagged(line, "pid=");
        const auto rowPerms = fsuser::tests::readTagged(line, "perms=", 0);
        if (!rowNode || !rowPid || !rowPerms || *rowNode != nodeId || *rowPid != 0)
        {
            continue;
        }
        return static_cast<std::uint32_t>(*rowPerms);
    }
    return std::nullopt;
}

// mount(2) writes this node's account row and narrows the region's default in the same call, and a
// refused row fails the mount, so a live mount carries both or is not there.
void checkBoundDefault(fsuser::tests::Report& report, const fsuser::tests::Mount& mount)
{
    report.section("what mount(2) leaves on the lock region");

    if (::geteuid() != 0)
    {
        report.note("the account row itself needs root to read, so only the default was checked");
    }
    else
    {
        const auto granted = readLockAccountPerms(mount.getNodeId());
        report.check("the lock region carries this node's account row", granted.has_value());
        if (granted.has_value())
        {
            const auto both = static_cast<std::uint32_t>(FS_PERM_READ) |
                              static_cast<std::uint32_t>(FS_PERM_WRITE);
            report.check("that row admits the daemon account to read and write",
                         (*granted & both) == both);
        }
    }

    const auto standing = readRegionDefault(FS_LOCK_REGION_RAT_ID);
    report.check("perm_info carries a row for the lock region", standing.has_value());
    if (!standing.has_value())
    {
        return;
    }

    report.check("the lock region's default admits READ and nothing more",
                 *standing == static_cast<std::uint32_t>(FS_PERM_READ));
    report.check("the default carries no WRITE bit, which is what the bind narrows",
                 (*standing & static_cast<std::uint32_t>(FS_PERM_WRITE)) == 0);
}

// The default this region ships with admits a read mapping. mprotect and a fresh write mapping
// both take the row-only guard that file_vma_mprotect enforces, with no upcall to widen it.
void checkWriteMapping(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("a write mapping past the lock region's default");

    auto region = fsuser::File::open(mount.pathTo(FS_LOCK_REGION_NAME), O_RDWR);

    fsuser::Mapping mapping;
    report.checkAccepted("mmap(PROT_READ) succeeds since the default admits READ",
                         [&]
                         {
                             mapping = region.map(PlacementSize, PROT_READ);
                         });
    if (!mapping.isMapped())
    {
        return;
    }

    errno = 0;
    const std::int32_t protectResult =
        ::mprotect(mapping.get(), static_cast<std::size_t>(mapping.getSize()),
                   PROT_READ | PROT_WRITE);
    report.checkErrno("mprotect to PROT_READ|PROT_WRITE on that mapping answers EACCES",
                      protectResult < 0 && errno == EACCES, errno);

    report.checkRefusedWith("a fresh mmap(PROT_READ|PROT_WRITE) answers EACCES",
                            std::errc::permission_denied,
                            [&]
                            {
                                (void)region.map(PlacementSize, PROT_READ | PROT_WRITE);
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());

        checkCreateUnlinkRename(report, mount);
        checkGrantRevoke(report, mount);
        checkBoundDefault(report, mount);
        checkWriteMapping(report, mount);
    };
    return fsuser::tests::runCase(argc, argv, "test_lock_region_guard", 1, body);
}
