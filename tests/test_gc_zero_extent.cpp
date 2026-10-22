// SPDX-License-Identifier: Apache-2.0
//
// test_gc_zero_extent -- a region the sweep takes from a dead owner is zero before the next file lands.
//
// A live owner clears what it wants cleared before it unlinks. A dead one cleared nothing, so the
// sweep zeroes the extent before it publishes the slot free, and the claim measured here is the one
// a consumer cares about: the first reader of a region placed over that extent sees zeros and not the
// dead owner's bytes.
//
// The second file has to land on the first one's extent for the zero check to be about the sweep at
// all. Placement is first-fit, so with nothing else placing meanwhile the freed extent is the lowest
// gap and the next placement takes it. The case reports whether that happened and runs serially.
//
// The same claim is made once for a region the kernel places and once for one asked onto the second
// device, which runs only when the mount has one.
//
//   test_gc_zero_extent <mount-a>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "fs/file.hpp"
#include "harness/device.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::findRowByName;
using fsuser::tests::PlacementSize;
using fsuser::tests::placeOnDevice;
using fsuser::tests::readSweepCount;
using fsuser::tests::RegionRow_t;
using fsuser::tests::waitForSlotFree;
using fsuser::tests::waitForSweeps;

constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::uint32_t FirstSeed = 0x5EEDBEEFU;

// How many words the read() check compares. The mapping check covers the whole span, and read()
// walks the same bytes through the kernel's own mapping.
constexpr std::uint64_t ReadWords = 16;

// Runs in the child. Places the name, fills it, and leaves without unlinking, which is the state the
// sweep is there to find.
[[noreturn]] void fillAndLeave(fsuser::tests::Mount& mount, const std::string& name, std::uint32_t device)
{
    try
    {
        auto file = placeOnDevice(mount, name, device, PlacementSize);
        auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, FirstSeed);
    }
    catch (const std::exception&)
    {
        ::_exit(1);
    }
    ::_exit(0);
}

// Every word of @words zero, naming the first that is not.
void checkAllZero(fsuser::tests::Report& report, std::string_view what, const std::uint32_t* words,
                  std::uint64_t count)
{
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (words[index] != 0)
        {
            report.check(std::string{what} + ": word " + std::to_string(index) + " reads " +
                             std::to_string(words[index]),
                         false);
            return;
        }
    }
    report.check(what, true);
}

// The dead owner's file, as region_info showed it before the sweep took it, or nothing when the setup
// did not get that far.
[[nodiscard]] std::optional<RegionRow_t> leaveFilledRegion(fsuser::tests::Report& report,
                                                           fsuser::tests::Mount& mount, const std::string& name,
                                                           std::uint32_t device)
{
    const ::pid_t child = ::fork();
    if (child == 0)
    {
        fillAndLeave(mount, name, device);
    }
    report.check("the owner starts", child > 0);
    if (child <= 0)
    {
        return std::nullopt;
    }
    std::int32_t status = 0;
    report.check("and leaves the filled file behind",
                 ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    auto row = findRowByName(name);
    report.check("region_info shows the file's extent", row && !fsuser::tests::readPiecesOf(name).empty());
    return row;
}

void checkReclaimedExtentIsZero(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                std::uint32_t device)
{
    const auto first = caseName("gc_zero_a").append("_").append(std::to_string(device));
    const auto second = caseName("gc_zero_b").append("_").append(std::to_string(device));
    report.section(std::string{"a dead owner leaves a filled region, device "}.append(std::to_string(device)));

    const auto ownerNode = mount.getNodeId();
    const auto before = readSweepCount(ownerNode);
    const auto firstRow = leaveFilledRegion(report, mount, first, device);
    if (!firstRow)
    {
        return;
    }
    const auto firstPieces = fsuser::tests::readPiecesOf(first);

    report.section("the sweep takes it back");

    report.check("the owning node sweeps twice", before >= 0 && waitForSweeps(ownerNode, before));
    report.check("and the slot is free", waitForSlotFree(firstRow->entry));

    report.section("the next file over the same extent reads zero");

    auto file = placeOnDevice(mount, second, device, PlacementSize);
    const bool sameExtent = !firstPieces.empty() && fsuser::tests::readPiecesOf(second) == firstPieces;
    report.check("the second file lands on the first one's extent", sameExtent);
    if (!sameExtent)
    {
        report.note("the zero checks below are about untouched bytes and not about the sweep");
    }

    {
        auto mapping = file.map(PlacementSize, PROT_READ);
        report.check("the second file maps readably", mapping.isMapped());
        checkAllZero(report, "every word of the mapping reads zero",
                     static_cast<const std::uint32_t*>(mapping.get()), PlacedWords);
    }

    std::uint32_t taken[ReadWords] = {};
    const std::int64_t got = ::pread(file.get(), taken, sizeof(taken), 0);
    report.check("read returns the bytes it was asked for", got == static_cast<std::int64_t>(sizeof(taken)));
    if (got == static_cast<std::int64_t>(sizeof(taken)))
    {
        checkAllZero(report, "read returns zeros as well", taken, ReadWords);
    }
    mount.unlink(second);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkReclaimedExtentIsZero(report, mount, fsuser::AnyDevice);

        if (!fsuser::tests::secondDevicePool())
        {
            report.note(fsuser::tests::NoSecondDeviceNote);
            return;
        }
        checkReclaimedExtentIsZero(report, mount, fsuser::tests::SecondDeviceAttr);
    };
    return fsuser::tests::runCase(argc, argv, "test_gc_zero_extent", 1, body);
}
