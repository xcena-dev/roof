// SPDX-License-Identifier: Apache-2.0
//
// test_perm_grants -- what one node's own file accepts: placement, and the arguments a grant takes.
//
// One mount, because nothing here needs a peer to observe it. What a grant does for a peer is the
// other case; what this one asks is which grants the kernel writes down at all.
//
//   test_perm_grants <mount-a>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstdint>
#include <exception>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

// Three of the delegation slots go to the named grants below, and the bulk fill takes the rest.
// The refused ones write nothing, so they do not count.
constexpr std::uint32_t NamedGrants = 3;

// Well clear of any account this host has, so a grant here names nobody who could then use it.
constexpr std::uint32_t FirstUid = 60000;
constexpr std::uint32_t FirstGid = 61000;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// A file arrives with no extent and gets one exactly once. The second resize has to be refused,
// which is what makes a placed region worth trusting.
void checkPlacement(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                    fsuser::File& file)
{
    report.section("placement happens once");

    struct ::stat placed = {};
    report.check("fstat before placement succeeds", ::fstat(file.get(), &placed) == 0);
    report.check("a fresh file has no size", placed.st_size == 0);

    report.checkAccepted("resize places the extent", [&]
                         {
                             file.resize(PlacementSize);
                         });

    report.check("fstat after placement succeeds", ::fstat(file.get(), &placed) == 0);
    report.check("the size is what was asked for",
                 static_cast<std::uint64_t>(placed.st_size) == PlacementSize);

    report.checkRefused("a second resize is refused",
                        [&]
                        {
                            file.resize(PlacementSize * 2);
                        });
    (void)mount;
}

// A delegation names an account on the node the call is made from. Either id alone is enough,
// naming neither would name everyone, and the mask has to be bits the kernel defines or a caller
// could hand out a permission that does not exist.
void checkGrantArguments(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("what a grant will and will not name");

    report.checkAccepted("grant READ to a uid",
                         [&]
                         {
                             file.grantPermission(FirstUid, fsuser::AnyId, fsuser::Permission::Read);
                         });
    report.checkAccepted("grant READ|WRITE to a gid", [&]
                         {
                             file.grantPermission(fsuser::AnyId, FirstGid, fsuser::Permission::Read | fsuser::Permission::Write);
                         });
    report.checkAccepted("grant ADMIN|READ to a uid and gid together", [&]
                         {
                             file.grantPermission(FirstUid + 1, FirstGid + 1, fsuser::Permission::Admin | fsuser::Permission::Read);
                         });

    // The same account again rewrites its own row rather than taking a second slot, which is what
    // lets a bring-up rerun without exhausting the table.
    report.checkAccepted("granting the same account again is accepted",
                         [&]
                         {
                             file.grantPermission(FirstUid, fsuser::AnyId, fsuser::Permission::Ioctl);
                         });

    report.checkRefused("naming neither a uid nor a gid is refused",
                        [&]
                        {
                            file.grantPermission(fsuser::AnyId, fsuser::AnyId, fsuser::Permission::Read);
                        });
    report.checkRefused("an empty mask is refused", [&]
                        {
                            file.grantPermission(1, 1, fsuser::Permission::None);
                        });
    report.checkRefused("a mask outside fsuser::Permission::All is refused",
                        [&]
                        {
                            file.grantPermission(1, 1, fsuser::Permission{0xFFFFU});
                        });
}

// The default is what a peer with no delegation of its own gets, and the owner may move it either
// way.
void checkDefaults(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("the default a peer inherits");

    report.checkAccepted("set the default to READ",
                         [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::Read);
                         });
    report.checkAccepted("set the default back to owner-only",
                         [&]
                         {
                             file.setDefaultPermission(fsuser::Permission::None);
                         });
}

// The table holds MaxDelegations rows and the grants above took three of them, so the rest have
// to fit and the one after them must not. Each grant names an account of its own, because a
// rerun for one already there rewrites its row instead of taking a slot.
void checkTableFills(fsuser::tests::Report& report, fsuser::File& file)
{
    report.section("the delegation table fills and then refuses");

    const std::uint32_t remaining = fsuser::MaxDelegations - NamedGrants;
    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < remaining; ++index)
    {
        try
        {
            file.grantPermission(FirstUid + 10 + index, fsuser::AnyId, fsuser::Permission::Read);
            ++written;
        }
        catch (const std::exception&)
        {
            break;
        }
    }
    report.check("every remaining slot takes a grant", written == remaining);

    report.checkRefusedWith("the grant past the last slot answers ENOSPC",
                            std::errc::no_space_on_device,
                            [&]
                            {
                                file.grantPermission(FirstUid + 900, fsuser::AnyId, fsuser::Permission::Read);
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto name = caseName("perm_grants");

        auto mount = fsuser::tests::Mount(mounts.first());
        auto file = mount.open(name, O_CREAT | O_RDWR, 0644);

        checkPlacement(report, mount, file);
        checkGrantArguments(report, file);
        checkDefaults(report, file);
        checkTableFills(report, file);

        mount.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_perm_grants", 1, body);
}
