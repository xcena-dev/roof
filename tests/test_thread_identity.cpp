// SPDX-License-Identifier: Apache-2.0
//
// test_thread_identity -- a thread is the same principal as the process it runs in, so the row its
// leader earned answers for it and the helper is not asked a second time.
//
// A delegation row names a tgid, which is the leader's pid, and the start time that goes with it.
// A thread starts later than its leader and so carries a different start time of its own. If that
// is what the row binds, then every thread of one process is a principal of its own, each one
// costing an upcall and a slot.
//
// The helper's asked count is the observable. Both binds are granted either way, so the count is
// what separates a row that answered from a helper that was asked again.
//
//   test_thread_identity <mount-a> <mount-b>

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// A delegation row is matched only from the node it names, so both binds reach the file through
// the peer's mount and the row is earned there.
void checkAThreadReusesItsLeadersRow(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                     const std::string& peerMount, const std::string& name)
{
    report.section("a thread created after its leader has bound");

    auto file = fsuser::tests::placeFile(mount, name);

    // Node id is a property of the mount table rather than of the mount point's name, so it is
    // read once and used for both counter reads.
    const auto peerNode = fsuser::tests::Mount(peerMount).getNodeId();

    const std::int32_t leaderResult =
        fsuser::tests::openAndProbeReadMap(peerMount, name, PlacementSize);
    report.checkErrno("the leader maps and so earns a row", leaderResult == 0, leaderResult);
    if (leaderResult != 0)
    {
        mount.unlink(name);
        return;
    }

    const auto askedBefore = fsuser::tests::readDaemonAsked(peerNode);
    report.check("the node's channel reports what it has been asked", askedBefore.has_value());

    std::int32_t threadResult = -1;
    std::thread mapper{[&threadResult, &peerMount, &name]
                       {
                           threadResult =
                               fsuser::tests::openAndProbeReadMap(peerMount, name, PlacementSize);
                       }};
    mapper.join();

    report.checkErrno("a thread of the same process maps", threadResult == 0, threadResult);

    const auto askedAfter = fsuser::tests::readDaemonAsked(peerNode);
    report.check("the leader's row answers the thread rather than the helper being asked again",
                 askedBefore && askedAfter && *askedAfter == *askedBefore);

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkAThreadReusesItsLeadersRow(report, mount, mounts.second(),
                                        caseName("thread_identity"));
    };
    return fsuser::tests::runCase(argc, argv, "test_thread_identity", 2, body);
}
