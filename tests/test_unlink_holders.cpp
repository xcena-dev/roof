// SPDX-License-Identifier: Apache-2.0
//
// test_unlink_holders -- when an unlink frees the extent at once, and when the holders decide.
//
// The name goes the moment unlink returns. The extent goes at once only when no process on any node
// holds the file open or mapped. Otherwise every holder keeps reading its own bytes, no placement
// lands on the extent, a new open of the name is refused, and the slot is freed once
// the last holder is gone. The holders here are forked processes on both mounts, and the case lets
// them go one at a time to show that the last one is what decides.
//
//   test_unlink_holders <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

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

constexpr std::uint32_t FirstSeed = 0x5A5A5A5AU;
constexpr std::uint32_t SecondSeed = 0xA5A5A5A5U;

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::Holder;
using fsuser::tests::nameExists;
using fsuser::tests::RegionRow_t;
using fsuser::tests::slotIsTaken;
using fsuser::tests::waitForSlotFree;

// Places @name, fills it with FirstSeed and closes it, so the holders that follow are the only
// references. Answers its row.
[[nodiscard]] std::optional<RegionRow_t> placeAndClose(fsuser::tests::Report& report,
                                                       fsuser::tests::Mount& mount, const std::string& name)
{
    {
        auto file = fsuser::tests::placeFile(mount, name);
        auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the file maps writably", stored.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, FirstSeed);
    }
    auto row = findRowByName(name);
    report.check("region_info lists the file", row.has_value());
    return row;
}

void checkNoHolderFreesAtOnce(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                              const std::string& name)
{
    report.section("no holder: the extent goes with the name");

    const auto row = placeAndClose(report, mount, name);
    report.checkAccepted("unlink goes through",
                         [&]
                         {
                             mount.unlink(name);
                         });
    report.check("the name is gone", !nameExists(mount.getPoint(), name));
    report.check("the slot is free at once, with no sweep in between", row && !slotIsTaken(row->entry));

    const auto again = placeAndClose(report, mount, name);
    report.check("the same name can be made again", again.has_value());
    if (row && again)
    {
        report.note(again->entry == row->entry ? "and it took the same slot" : "and it took another slot");
    }
    mount.unlink(name);
}

void checkOneHolderDefers(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                          fsuser::tests::Mount& peer, const std::string& name)
{
    report.section("one holder on this node, another process");

    const auto row = placeAndClose(report, owner, name);
    Holder holder;
    holder.start(owner.getPoint(), name, FirstSeed);
    report.check("the holder starts", holder.started());
    report.check("and maps the file", holder.mapped() == 1);

    // The owner's own descriptor, opened after the fork so the holder did not inherit it.
    auto file = owner.open(name, O_RDWR);

    report.checkAccepted("unlink goes through",
                         [&]
                         {
                             owner.unlink(name);
                         });
    report.check("the name is gone", !nameExists(owner.getPoint(), name));
    report.check("the slot stays taken", row && slotIsTaken(row->entry));
    report.check("the holder still reads its own bytes", holder.check() == 1);

    report.section("what the gone name still allows");
    report.checkRefusedWith("a new open of the name on this mount is refused", std::errc::no_such_file_or_directory,
                            [&]
                            {
                                auto opened = owner.open(name, O_RDONLY);
                            });
    report.checkRefusedWith("and on the other mount", std::errc::no_such_file_or_directory,
                            [&]
                            {
                                auto opened = peer.open(name, O_RDONLY);
                            });
    {
        auto again = file.map(PlacementSize, PROT_READ);
        report.check("the open descriptor still maps", again.isMapped());
        fsuser::tests::checkPattern(report, "and the new mapping reads the file's own bytes",
                                    static_cast<const std::uint32_t*>(again.get()), PlacedWords, FirstSeed);
    }

    report.check("the holder leaves cleanly", holder.leave());
    report.check("the slot stays taken while the owner's descriptor is open", row && slotIsTaken(row->entry));
    {
        auto closing = std::move(file);
    }
    report.check("the slot is free once the last descriptor closed", row && waitForSlotFree(row->entry));
}

void checkLastHolderDecides(fsuser::tests::Report& report, fsuser::tests::Mount& owner, fsuser::tests::Mount& peer,
                            const std::string& name, const std::string& secondName)
{
    report.section("four holders on two nodes");

    const auto row = placeAndClose(report, owner, name);
    Holder onThisNode;
    Holder firstOnPeer;
    Holder secondOnPeer;
    onThisNode.start(owner.getPoint(), name, FirstSeed);
    firstOnPeer.start(peer.getPoint(), name, FirstSeed);
    secondOnPeer.start(peer.getPoint(), name, FirstSeed);
    report.check("three holders start", onThisNode.started() && firstOnPeer.started() && secondOnPeer.started());
    report.check("and all three map the file",
                 onThisNode.mapped() == 1 && firstOnPeer.mapped() == 1 && secondOnPeer.mapped() == 1);
    auto file = owner.open(name, O_RDWR);

    report.checkAccepted("unlink goes through",
                         [&]
                         {
                             owner.unlink(name);
                         });
    report.check("the name is gone from both mounts",
                 !nameExists(owner.getPoint(), name) && !nameExists(peer.getPoint(), name));
    report.check("the slot stays taken", row && slotIsTaken(row->entry));
    report.check("every holder still reads its own bytes",
                 onThisNode.check() == 1 && firstOnPeer.check() == 1 && secondOnPeer.check() == 1);

    report.section("holders leave one at a time");

    report.check("the holder on this node leaves", onThisNode.leave());
    report.check("three references left: the slot stays taken", row && slotIsTaken(row->entry));

    {
        auto second = fsuser::tests::placeFile(owner, secondName);
        auto stored = second.map(PlacementSize, PROT_READ | PROT_WRITE);
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, SecondSeed);
        const auto secondRow = findRowByName(secondName);
        report.check("a file placed meanwhile lands on another extent",
                     row && secondRow && secondRow->offset != row->offset);
        owner.unlink(secondName);
    }

    report.check("the first peer holder leaves", firstOnPeer.leave());
    report.check("two references left: the slot stays taken", row && slotIsTaken(row->entry));
    {
        auto closing = std::move(file);
    }
    report.check("one reference left: the slot stays taken", row && slotIsTaken(row->entry));
    report.check("and the last holder still reads its own bytes", secondOnPeer.check() == 1);

    report.check("the last holder leaves", secondOnPeer.leave());
    report.check("no reference left: the slot is free", row && waitForSlotFree(row->entry));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());

        checkNoHolderFreesAtOnce(report, owner, caseName("holders_none"));
        checkOneHolderDefers(report, owner, peer, caseName("holders_one"));
        checkLastHolderDecides(report, owner, peer, caseName("holders_four"), caseName("holders_four2"));
    };
    return fsuser::tests::runCase(argc, argv, "test_unlink_holders", 2, body);
}
