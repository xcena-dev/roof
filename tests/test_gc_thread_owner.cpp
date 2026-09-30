// SPDX-License-Identifier: Apache-2.0
//
// test_gc_thread_owner -- a region a worker thread created belongs to the process it runs in, so the
// sweep finds an owner that is alive and the rest of the process reaches the region without asking.
//
// An owner record holds a tgid, which names the thread group leader, and the start time that goes
// beside it. A thread starts later than its leader and carries a start time of its own. The sweep
// asks whether the process that pid names is still there, and the only task a pid lookup can answer
// with is the leader, so a record carrying the creating thread's time reads as an owner that is
// gone while the process is still running.
//
// Two observables, one per section. The name a thread made is still there once the sweep has run
// twice, and no reach into that region from the same process costs a helper decision.
//
//   test_gc_thread_owner <mount>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::nameExists;
using fsuser::tests::PlacementSize;
using fsuser::tests::readSweepCount;
using fsuser::tests::waitForSweeps;

// Placed from a task that is not the thread group leader, which is the record this case is about.
// The throw is caught inside the thread, because one leaving it would end the run rather than a
// single check.
[[nodiscard]] bool placeFromAThread(fsuser::tests::Mount& mount, const std::string& name)
{
    bool placed = false;
    std::thread maker{[&placed, &mount, &name]
                      {
                          try
                          {
                              auto file = fsuser::tests::placeFile(mount, name);
                              placed = true;
                          }
                          catch (const std::exception&)
                          {
                              placed = false;
                          }
                      }};
    maker.join();
    return placed;
}

// The owner path is the node's own, so both reaches go through the mount the region was created on.
// Either one taking an upcall is the owner record naming a task rather than the process.
void checkTheProcessOwnsWhatItsThreadMade(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                          const std::string& name)
{
    report.section("what a thread made, the process owns");

    report.check("a thread of this process places the region", placeFromAThread(mount, name));
    if (!nameExists(mount.getPoint(), name))
    {
        return;
    }

    const auto node = mount.getNodeId();
    const auto askedBefore = fsuser::tests::readDaemonAsked(node);
    report.check("the node's channel reports what it has been asked", askedBefore.has_value());

    const std::int32_t leaderResult =
        fsuser::tests::openAndProbeReadMap(mount.getPoint(), name, PlacementSize);
    report.checkErrno("the leader maps the region its thread made", leaderResult == 0, leaderResult);

    std::int32_t otherResult = -1;
    std::thread reader{[&otherResult, &mount, &name]
                       {
                           otherResult = fsuser::tests::openAndProbeReadMap(mount.getPoint(), name,
                                                                            PlacementSize);
                       }};
    reader.join();
    report.checkErrno("and a thread made after it maps the same region", otherResult == 0,
                      otherResult);

    const auto askedAfter = fsuser::tests::readDaemonAsked(node);
    report.check(
        "neither reach was a decision for the helper, the owner record having named the "
        "process",
        askedBefore && askedAfter && *askedAfter == *askedBefore);

    report.checkAccepted("the leader takes the region back down", [&mount, &name]
                         {
                             mount.unlink(name);
                         });
}

// The leader's name is the control: it is owned the same way, so the sweep taking only the thread's
// would be the owner record and not the sweep.
void checkTheSweepLeavesItStanding(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                   const std::string& threadName, const std::string& leaderName)
{
    report.section("what the sweep makes of an owner it has to look up");

    report.check("a thread of this process places the region the sweep will judge",
                 placeFromAThread(mount, threadName));
    report.checkAccepted("and the leader places one beside it",
                         [&mount, &leaderName]
                         {
                             auto file = fsuser::tests::placeFile(mount, leaderName);
                         });

    const auto node = mount.getNodeId();
    const auto before = readSweepCount(node);
    report.check("gc_status reports a sweep count for this node: " + std::to_string(before),
                 before >= 0);
    report.check("the node sweeps twice within the deadline", before >= 0 && waitForSweeps(node, before));

    report.check("the name a thread made is still there, its owner being a process that is running",
                 nameExists(mount.getPoint(), threadName));
    report.check("and so is the one the leader made", nameExists(mount.getPoint(), leaderName));

    for (const auto& left : {threadName, leaderName})
    {
        if (nameExists(mount.getPoint(), left))
        {
            report.checkAccepted("the name goes back down: " + left,
                                 [&mount, &left]
                                 {
                                     mount.unlink(left);
                                 });
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    // The kernel attests the real uid, and a run under sudo would offer one the daemon's rules need
    // not name. A run that is already the invoking account has nothing to drop and carries on.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkTheProcessOwnsWhatItsThreadMade(report, mount, caseName("thread_owner_reach"));
        checkTheSweepLeavesItStanding(report, mount, caseName("thread_owner_swept"),
                                      caseName("thread_owner_control"));
    };
    return fsuser::tests::runCase(argc, argv, "test_gc_thread_owner", 1, body);
}
