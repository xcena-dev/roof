// SPDX-License-Identifier: Apache-2.0
//
// test_gc_deleg -- a delegation row outlives the process it named, and the GC is what takes it back.
//
// Refusing a dead process is one thing and reclaiming its slot is another. A row whose start time no
// longer matches stops granting anything the moment the process is gone, which is what test_pid_reuse
// shows. The row itself still occupies one of the region's delegation slots until a sweep clears it,
// and a region that never got the slot back would refuse every later grant.
//
// So the slot is what this case watches, through the only surface that reports it: the table is
// filled until it refuses, the granted process is killed, and one grant has to fit afterwards.
//
// Exactly one, which is the second half of the claim. The filler rows name pids that never touched
// the region, so their start time is unbound and the sweep leaves them alone for a stale timeout well
// past this case's runtime. A second grant fitting would mean the sweep took more than it should.
//
// A row is only ever cleared by the thread of the node it names, so the sweep counter this waits on is
// the peer node's, and the sysfs gc_status is where that counter is published.
//
//   test_gc_deleg <mount-a> <mount-b>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "fs/testing.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

// Well clear of any account this host has, so the fill names nobody who could then use it.
constexpr std::uint32_t FirstFillUid = 63000;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;
using fsuser::tests::readSweepCount;
using fsuser::tests::waitForSweeps;

// Runs in the child. Maps once, which is what binds this process's start time into the row it was
// granted, then blocks until it is killed. Answers the mapping's errno over @handoff's ack first.
[[noreturn]] void bindAndWait(fsuser::tests::Handoff_t& handoff, const std::string& mount,
                              const std::string& name)
{
    static_cast<void>(handoff.begin.take());

    const std::int32_t taken = fsuser::testing::openRaw(mount, name, O_RDWR | O_CLOEXEC);
    if (taken < 0)
    {
        handoff.ack.pass(errno);
        ::_exit(1);
    }
    handoff.ack.pass(fsuser::tests::probeReadMap(taken, PlacementSize));
    ::close(taken);

    // Nothing ever passes this, and nothing is meant to: the row has to still name a living process
    // when the parent fills the table.
    static_cast<void>(handoff.begin.take());
    ::_exit(0);
}

// Grants until the table refuses, and answers how many landed. Every row names an account, which
// is a row the sweep never takes back: it holds no process to outlive. So what comes back below
// is the child's row and nothing else.
[[nodiscard]] std::int32_t fillRemainingSlots(fsuser::File& file, std::uint32_t firstUid)
{
    std::int32_t written = 0;
    for (std::uint32_t index = 0; index < fsuser::MaxDelegations; ++index)
    {
        try
        {
            file.grantPermission(firstUid + index, fsuser::AnyId, fsuser::Permission::Read);
            ++written;
        }
        catch (const std::exception&)
        {
            break;
        }
    }
    return written;
}

void checkTheSlotComesBack(fsuser::tests::Report& report, fsuser::tests::Handoff_t& handoff,
                           fsuser::tests::Mount& mount, const std::string& peerMount,
                           const std::string& name)
{
    auto file = fsuser::tests::placeFile(mount, name);
    file.setDefaultPermission(fsuser::Permission::None);

    report.section("a delegated process binds its row");

    const ::pid_t granted = ::fork();
    if (granted == 0)
    {
        bindAndWait(handoff, peerMount, name);
    }
    report.check("fork succeeds", granted > 0);
    if (granted <= 0)
    {
        return;
    }

    // No grant from here. The child's map is refused on the record, the helper on its node
    // decides, and the row the kernel then writes names that child and its start time.
    handoff.begin.pass(1);
    const std::int32_t bound = handoff.ack.take();
    report.checkErrno("the child maps and so earns a row of its own", bound == 0, bound);

    report.section("the table is filled and then loses its holder");

    const std::int32_t filled = fillRemainingSlots(file, FirstFillUid);
    report.check("the remaining slots all took a grant: " + std::to_string(filled) + " of " +
                     std::to_string(fsuser::MaxDelegations - 1),
                 filled == static_cast<std::int32_t>(fsuser::MaxDelegations) - 1);
    // An account the fill never wrote, so the same request is refused while the table is full and
    // taken once a slot comes back. Naming an account already in the table would rewrite that row
    // instead of asking for a slot at all.
    const std::uint32_t probeUid = FirstFillUid + fsuser::MaxDelegations + 1;
    report.checkRefusedWith("one more grant answers ENOSPC", std::errc::no_space_on_device,
                            [&]
                            {
                                file.grantPermission(probeUid, fsuser::AnyId, fsuser::Permission::Read);
                            });

    // The peer's thread is what sweeps the row the child earned, so its count is the one watched.
    const auto peerNode = fsuser::tests::Mount(peerMount).getNodeId();
    const auto before = readSweepCount(peerNode);
    report.check("the peer node publishes a sweep count", before >= 0);

    report.check("the child is killed", ::kill(granted, SIGKILL) == 0);
    std::int32_t status = 0;
    report.check("the child is reaped as killed",
                 ::waitpid(granted, &status, 0) == granted && (WIFSIGNALED(status) != 0));

    report.section("what the sweep gives back");

    report.check("the peer node sweeps twice within the deadline",
                 before >= 0 && waitForSweeps(peerNode, before));

    report.checkAccepted("the same grant now fits where the dead row was",
                         [&]
                         {
                             file.grantPermission(probeUid, fsuser::AnyId, fsuser::Permission::Read);
                         });
    report.checkRefusedWith("that one slot is all the sweep gave back",
                            std::errc::no_space_on_device,
                            [&]
                            {
                                file.grantPermission(probeUid + 1, fsuser::AnyId, fsuser::Permission::Read);
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 2;
    }

    if (readSweepCount(fsuser::tests::Mount(mounts.second()).getNodeId()) < 0)
    {
        std::printf("test_gc_deleg: skipped, %s does not report the peer node's sweep count\n",
                    fsuser::tests::GcStatusPath);
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    const auto name = caseName("gc_deleg");

    fsuser::tests::Handoff_t handoff;
    if (!handoff.isOpen())
    {
        report.check("the handoff pipes open", false);
        return report.summarise("test_gc_deleg");
    }

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkTheSlotComesBack(report, handoff, mount, mounts.second(), name);
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_gc_deleg setup", failure);
    }

    return report.summarise("test_gc_deleg");
}
