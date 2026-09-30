// SPDX-License-Identifier: Apache-2.0
//
// test_dead_owner_unlink -- forcing an unlink off a region whose owner process died.
// gc_can_force_unlink allows this only from the node the entry names as owner, never by caller identity.
//
//   test_dead_owner_unlink <mount-a> <mount-b>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <system_error>

#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::nameExists;

// Runs in the child. Places the name with the default permission left at none, then exits without
// unlinking, which is the state acl_check_permission and gc_can_force_unlink are asked about below.
[[noreturn]] void makeAndLeave(fsuser::tests::Mount& mount, const std::string& name)
{
    try
    {
        auto file = fsuser::tests::placeFile(mount, name);
    }
    catch (const std::exception&)
    {
        ::_exit(1);
    }
    ::_exit(0);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 2;
    }

    fsuser::tests::Report report;
    report.section("a region whose owner exited without unlinking");

    const auto name = caseName("dead_owner_unlink");

    const ::pid_t child = ::fork();
    if (child == 0)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        makeAndLeave(mount, name);
    }
    report.check("the child that makes the name starts", child > 0);
    if (child <= 0)
    {
        return report.summarise("test_dead_owner_unlink");
    }

    std::int32_t status = 0;
    report.check("and finishes", ::waitpid(child, &status, 0) == child);
    report.check("having made the name without an error",
                 WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Acting the moment the child is reaped is what puts this process ahead of the sweep, which
    // otherwise runs every ten seconds and would take the name before either unlink below does.
    if (!nameExists(mounts.first(), name))
    {
        report.note("the sweep took the name before either unlink ran, skipping the checks below");
        return report.summarise("test_dead_owner_unlink");
    }

    report.section("who may force the unlink");

    fsuser::tests::Mount peer(mounts.second());
    report.checkRefusedWith("a node that is not the owner cannot force the unlink",
                            std::errc::permission_denied,
                            [&]
                            {
                                peer.unlink(name);
                            });

    fsuser::tests::Mount owner(mounts.first());
    report.checkAccepted("the owning node forces the unlink though this caller is not the owner",
                         [&]
                         {
                             owner.unlink(name);
                         });

    report.check("the name is gone from the owning mount", !nameExists(mounts.first(), name));
    report.check("and gone from the peer mount as well", !nameExists(mounts.second(), name));

    return report.summarise("test_dead_owner_unlink");
}
