// SPDX-License-Identifier: Apache-2.0
//
// test_gc_owner -- a file outlives the process that made it, and the sweep is what takes it back.
//
// test_gc_deleg watches a delegation row, which is one slot inside a region that somebody else still
// holds. This watches the region itself: nothing delegated, nobody else holding it, and an owner that
// is gone.
//
// The owner has to be a process that exits. A RAT entry records its creator's pid and start time, and
// gc_is_orphaned asks whether that process is still there, so a name this case made itself would stay
// for as long as the case runs. The name is made in a forked child that exits, which is the only way
// to reach the state under test.
//
// The wait is on the sweep counter of the node that owns the entry, because a node reclaims only what
// its own node_id stamped. That is the first mount. The second mount is here to answer the other half:
// a name taken back is taken back for everyone, not just for the node that swept.
//
//   test_gc_owner <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>

#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::nameExists;
using fsuser::tests::readSweepCount;
using fsuser::tests::waitForSweeps;

// Runs in the child. Places the name so the entry is ALLOCATED with an extent rather than a bare
// reservation, then leaves without unlinking, which is the state the sweep is there to find.
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
    report.section("a name its maker did not outlive");

    const auto name = caseName("gc_owner");
    // The owning node's thread is what reclaims what a process there left behind.
    const auto ownerNode = fsuser::tests::Mount(mounts.first()).getNodeId();
    const auto before = readSweepCount(ownerNode);
    report.check("gc_status reports a sweep count for the owning node: " + std::to_string(before),
                 before >= 0);

    const ::pid_t child = ::fork();
    if (child == 0)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        makeAndLeave(mount, name);
    }
    report.check("the child that makes the name starts", child > 0);
    if (child <= 0)
    {
        return report.summarise("test_gc_owner");
    }

    std::int32_t status = 0;
    report.check("and finishes", ::waitpid(child, &status, 0) == child);
    report.check("having made the name without an error",
                 WIFEXITED(status) && WEXITSTATUS(status) == 0);

    report.check("the name is there once its maker is gone", nameExists(mounts.first(), name));
    report.check("and the other mount sees it too", nameExists(mounts.second(), name));

    report.section("what the sweep takes back");

    report.check("the owning node sweeps twice within the deadline",
                 before >= 0 && waitForSweeps(ownerNode, before));

    report.check("the name is gone from the mount that owned it", !nameExists(mounts.first(), name));
    report.check("and gone from the other mount as well", !nameExists(mounts.second(), name));

    // Only reached when the sweep did not take it, and then the name would outlive this run.
    if (nameExists(mounts.first(), name))
    {
        try
        {
            fsuser::tests::Mount(mounts.first()).unlink(name);
        }
        catch (const std::exception& failure)
        {
            report.raised("clearing the name the sweep left", failure);
        }
    }

    return report.summarise("test_gc_owner");
}
