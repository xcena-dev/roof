// SPDX-License-Identifier: Apache-2.0
//
// test_gc_grant_rows -- an account row and a role row do not keep a dead owner's region.
//
// Only a process row names a live holder, and it goes with that process. An account grant and a role
// grant name a class of future callers, so nothing is holding the region on their behalf and the
// sweep takes it back once its owner is gone.
//
//   test_gc_grant_rows <mount-a> <mount-b>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"

namespace
{

// Nobody holds this uid on a test host, and nobody needs to: the row only has to be written.
constexpr std::uint32_t AbsentUid = 62000;

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::nameExists;
using fsuser::tests::readSweepCount;
using fsuser::tests::slotIsTaken;
using fsuser::tests::waitForSlotFree;
using fsuser::tests::waitForSweeps;

// Runs in the child, which has to be the owner: a RAT entry records its creator's pid, and the sweep
// asks whether that process is still there. Answers 1 over @handoff once both grants stand and the
// descriptor is closed, then waits to be released.
[[noreturn]] void ownAndGrant(fsuser::tests::Handoff_t& handoff, const std::string& mountPoint,
                              const std::string& name)
{
    try
    {
        fsuser::tests::Mount mount{mountPoint};
        {
            auto file = fsuser::tests::placeFile(mount, name);
            file.setDefaultPermission(fsuser::Permission::None);
            file.grantPermission(AbsentUid, fsuser::AnyId, fsuser::Permission::Read);
            file.grantRole("travel", "hotel", fsuser::Permission::Read);
        }
        handoff.ack.pass(1);
        static_cast<void>(handoff.begin.take());
    }
    catch (const std::exception&)
    {
        handoff.ack.pass(0);
        ::_exit(1);
    }
    ::_exit(0);
}

void checkGrantsDoNotHold(fsuser::tests::Report& report, fsuser::tests::Mount& owner, const std::string& name)
{
    report.section("an owner that leaves an account row and a role row behind");

    fsuser::tests::Handoff_t handoff;
    report.check("the handoff pipes open", handoff.isOpen());

    const ::pid_t maker = ::fork();
    if (maker == 0)
    {
        ownAndGrant(handoff, owner.getPoint(), name);
    }
    report.check("fork succeeds", maker > 0);
    if (maker <= 0)
    {
        return;
    }

    report.check("the owner places the file and writes both grants", handoff.ack.take() == 1);
    const auto row = findRowByName(name);
    report.check("region_info lists the file", row.has_value());

    report.check("the slot is taken while the owner lives", row && slotIsTaken(row->entry));

    const auto ownerNode = owner.getNodeId();
    const auto before = readSweepCount(ownerNode);
    handoff.begin.pass(1);
    std::int32_t status = 0;
    const bool reaped = ::waitpid(maker, &status, 0) == maker;
    report.check("the owner exits without unlinking", reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    report.check("the owning node sweeps twice", before >= 0 && waitForSweeps(ownerNode, before));
    report.check("the slot is free, though both rows were written", row && waitForSlotFree(row->entry));
    report.check("and the name went with it", !nameExists(owner.getPoint(), name));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        checkGrantsDoNotHold(report, owner, caseName("gc_grant_rows"));
    };
    return fsuser::tests::runCase(argc, argv, "test_gc_grant_rows", 2, body);
}
