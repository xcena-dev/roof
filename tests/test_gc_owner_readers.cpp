// SPDX-License-Identifier: Apache-2.0
//
// test_gc_owner_readers -- a dead owner's region stays while a reader holds it, row or no row.
//
// test_gc_owner shows the sweep taking back a region nobody holds. Here somebody does: a process on
// the other node has it mapped. With a delegation row, the row already kept the region. Without one,
// reached through the default permission alone, only the reference count does. Either way the sweep
// waits for the reader, and takes the name and the slot once the reader is gone. A live process on
// the owning node may still unlink the name meanwhile, and that too frees nothing until the reader
// leaves.
//
//   test_gc_owner_readers <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/holder.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::PlacementSize;
constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::uint32_t Seed = 0x7E7E7E7EU;

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::Holder;
using fsuser::tests::nameExists;
using fsuser::tests::readSweepCount;
using fsuser::tests::RegionRow_t;
using fsuser::tests::slotIsTaken;
using fsuser::tests::waitForSlotFree;
using fsuser::tests::waitForSweeps;

// The owner has to be a process that exits: a RAT entry records its creator's pid and start time, and
// the sweep asks whether that process is still there.
class Owner
{
public:
    Owner(const std::string& mountPoint, const std::string& name, fsuser::Permission defaultPerms)
        : pid_{::fork()}
    {
        if (pid_ == 0)
        {
            make(mountPoint, name, defaultPerms);
        }
    }
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    ~Owner()
    {
        if (pid_ > 0)
        {
            die();
        }
    }

    [[nodiscard]] bool started() const noexcept
    {
        return pid_ > 0;
    }

    // 1 once the file is placed, filled and closed.
    [[nodiscard]] std::int32_t made()
    {
        return handoff_.ack.take();
    }

    // Lets the owner exit without unlinking, which is the state the sweep is there to find.
    bool die()
    {
        if (pid_ <= 0)
        {
            return false;
        }
        handoff_.begin.pass(1);
        std::int32_t status = 0;
        const bool reaped = ::waitpid(pid_, &status, 0) == pid_;
        pid_ = -1;
        return reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

private:
    [[noreturn]] void make(const std::string& mountPoint, const std::string& name, fsuser::Permission defaultPerms)
    {
        try
        {
            fsuser::tests::Mount mount{mountPoint};
            {
                auto file = fsuser::tests::placeFile(mount, name);
                auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
                fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, Seed);
                file.setDefaultPermission(defaultPerms);
            }
            handoff_.ack.pass(1);
            static_cast<void>(handoff_.begin.take());
        }
        catch (const std::exception&)
        {
            handoff_.ack.pass(0);
            ::_exit(1);
        }
        ::_exit(0);
    }

private:
    // Declared before the pid, so the pipes exist before the fork that the pid's initializer runs.
    fsuser::tests::Handoff_t handoff_;
    ::pid_t pid_{-1};
};

// The shape all three arms share: an owner that dies while a peer reads. Answers the row, or nothing
// when the setup did not get that far.
[[nodiscard]] std::optional<RegionRow_t> makeAndRead(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                                                     const std::string& name, Holder& reader, Owner& maker)
{
    report.check("the owner starts", maker.started());
    report.check("and makes the file", maker.made() == 1);
    auto row = findRowByName(name);
    report.check("region_info lists the file", row.has_value());

    reader.start(peer.getPoint(), name, Seed);
    report.check("the reader on the other node maps it", reader.started() && reader.mapped() == 1);
    return row;
}

// Two sweeps on the owning node after the owner died, which must leave the region standing.
void checkSweepsLeaveIt(fsuser::tests::Report& report, fsuser::tests::Mount& owner, Owner& maker,
                        const std::optional<RegionRow_t>& row, Holder& reader)
{
    const auto ownerNode = owner.getNodeId();
    const auto before = readSweepCount(ownerNode);
    report.check("the owner dies without unlinking", maker.die());
    report.check("the owning node sweeps twice", before >= 0 && waitForSweeps(ownerNode, before));

    report.check("the slot is still taken", row && slotIsTaken(row->entry));
    report.check("the reader still reads its own bytes", reader.check() == 1);
}

void checkReaderWithoutRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner, fsuser::tests::Mount& peer,
                           const std::string& name)
{
    report.section("a reader through the default permission, no row");

    Holder reader;
    Owner maker{owner.getPoint(), name, fsuser::Permission::Read};
    const auto row = makeAndRead(report, peer, name, reader, maker);
    checkSweepsLeaveIt(report, owner, maker, row, reader);
    report.check("the name is still there on both mounts", nameExists(owner.getPoint(), name) && nameExists(peer.getPoint(), name));

    report.check("the reader leaves", reader.leave());
    report.check("the slot is free once a sweep ran", row && waitForSlotFree(row->entry));
    report.check("and the name went with it", !nameExists(owner.getPoint(), name));
}

void checkReaderWithRow(fsuser::tests::Report& report, fsuser::tests::Mount& owner, fsuser::tests::Mount& peer,
                        const std::string& name)
{
    report.section("a reader the helper granted a row");

    Holder reader;
    Owner maker{owner.getPoint(), name, fsuser::Permission::None};
    const auto row = makeAndRead(report, peer, name, reader, maker);
    checkSweepsLeaveIt(report, owner, maker, row, reader);

    report.check("the reader leaves", reader.leave());
    report.check("the slot is free once the row and the reference are both gone", row && waitForSlotFree(row->entry));
    report.check("and the name went with it", !nameExists(owner.getPoint(), name));
}

void checkForceUnlinkDefers(fsuser::tests::Report& report, fsuser::tests::Mount& owner, fsuser::tests::Mount& peer,
                            const std::string& name)
{
    report.section("a live process on the owning node unlinks the dead owner's name");

    Holder reader;
    Owner maker{owner.getPoint(), name, fsuser::Permission::Read};
    const auto row = makeAndRead(report, peer, name, reader, maker);
    checkSweepsLeaveIt(report, owner, maker, row, reader);

    report.checkAccepted("the unlink goes through without DELETE, since the owner is dead",
                         [&]
                         {
                             owner.unlink(name);
                         });
    report.check("the name is gone from both mounts", !nameExists(owner.getPoint(), name) && !nameExists(peer.getPoint(), name));
    report.check("the slot stays taken for the reader", row && slotIsTaken(row->entry));
    report.check("who still reads its own bytes", reader.check() == 1);

    report.check("the reader leaves", reader.leave());
    report.check("the slot is free once a sweep ran", row && waitForSlotFree(row->entry));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        checkReaderWithoutRow(report, owner, peer, caseName("gc_readers_norow"));
        checkReaderWithRow(report, owner, peer, caseName("gc_readers_row"));
        checkForceUnlinkDefers(report, owner, peer, caseName("gc_readers_unlink"));
    };
    return fsuser::tests::runCase(argc, argv, "test_gc_owner_readers", 2, body);
}
