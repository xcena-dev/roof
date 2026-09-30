// SPDX-License-Identifier: Apache-2.0
//
// test_sysfs -- what the filesystem publishes about itself, and whether it agrees with what was done.
//
// Two of the files here are load-bearing for the rest of the suite. region_info is the only place a
// RAT slot's physical extent is visible, so a case that asks whether two placements collided has
// nowhere else to look. gc_status carries a sweep counter, which is what lets a case wait for a sweep
// rather than sleep for a guess.
//
// So the checks are two-sided: each file is read, and then something is done through the library and
// the file has to have followed. A file that reads cleanly but never changes would pass the first half
// and fail the second.
//
// The debug files that force a sweep or select a region belong to root, and nothing here needs them.
//
//   test_sysfs <mount-a> <mount-b>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"
#include "name.h"

namespace
{

// Which node each mount is. Both threads run on this host, so both counters have to move. Read
// from the mounts and not assumed, since a slot a recovery has yet to free moves a mount's id up.
struct MountNodes_t
{
    std::uint32_t first;
    std::uint32_t second;
};

// A sweep every few seconds is the observed cadence, and this is well past it so a slower host is a
// slower run rather than a red one.
constexpr std::int32_t SweepDeadlineSeconds = 60;
constexpr std::int32_t SweepPollMillis = 250;

// The one sysfs file that names every delegation row, rather than a count of them.
constexpr const char* DelegInfoPath = "/sys/fs/" FS_NAME_STR "/deleg_info";

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;
using fsuser::tests::RegionRow_t;

[[nodiscard]] const RegionRow_t* findRow(const std::vector<RegionRow_t>& rows,
                                         const std::string& name)
{
    const auto found = std::find_if(rows.begin(), rows.end(),
                                    [&name](const RegionRow_t& row)
                                    {
                                        return row.name == name;
                                    });
    return found == rows.end() ? nullptr : &*found;
}

void checkPermInfoIsReadable(fsuser::tests::Report& report)
{
    report.section("the file that only has to be readable");

    const std::ifstream perms{fsuser::tests::PermInfoPath};
    report.check("perm_info opens for reading", static_cast<bool>(perms));
}

// deleg_info names every row a region carries, unlike perm_info's counts, so only its owner reads it.
void checkDelegInfoIsRootOnly(fsuser::tests::Report& report)
{
    report.section("the file that only root may read");

    if (::geteuid() == 0)
    {
        report.note("running as root, so deleg_info's own owner check was skipped");
        return;
    }

    errno = 0;
    const std::int32_t opened = ::open(DelegInfoPath, O_RDONLY);
    report.checkErrno("deleg_info is refused to a non-root reader",
                      opened < 0 && errno == EACCES, errno);
    if (opened >= 0)
    {
        ::close(opened);
    }
}

void checkRegionInfoFollowsTheRegion(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                     const std::string& name)
{
    report.section("region_info against what the library just did");

    const auto before = fsuser::tests::readRegionInfo();
    report.check("region_info opens for reading", before.has_value());
    report.check("its header names the columns this suite reads",
                 before && before->header == fsuser::tests::RegionInfoHeader);
    report.check("the name is not among its rows yet",
                 before && findRow(before->rows, name) == nullptr);

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);

    const std::vector<RegionRow_t> created = fsuser::tests::readRegionRows();
    const RegionRow_t* fresh = findRow(created, name);
    report.check("creating the name adds a row for it", fresh != nullptr);
    if (fresh != nullptr)
    {
        report.check("the row is ALLOCATED: \"" + fresh->state + "\"",
                     fresh->state == "ALLOCATED");
        report.check("it reports this process as the owner",
                     fresh->pid == static_cast<std::uint32_t>(::getpid()));
        report.check("it carries no extent before placement",
                     fresh->size == 0 && fresh->offset == 0);
        report.check("exactly one row carries the name",
                     fsuser::tests::countRegionsNamed(name) == 1);
    }

    file.resize(PlacementSize);

    const std::vector<RegionRow_t> placed = fsuser::tests::readRegionRows();
    const RegionRow_t* extent = findRow(placed, name);
    report.check("the row survives placement", extent != nullptr);
    if (extent != nullptr)
    {
        report.check("its size is what was placed", extent->size == PlacementSize);
        report.check("its offset is no longer zero", extent->offset != 0);
    }

    mount.unlink(name);
    report.check("removing the name removes its row",
                 findRow(fsuser::tests::readRegionRows(), name) == nullptr);
}

void checkTheSweepCounterMoves(fsuser::tests::Report& report, const MountNodes_t& nodes)
{
    report.section("gc_status, which the suite waits on");

    const auto firstBefore = fsuser::tests::readSweepCount(nodes.first);
    const auto secondBefore = fsuser::tests::readSweepCount(nodes.second);
    report.check("gc_status reports a count for the first mount's node: " +
                     std::to_string(firstBefore),
                 firstBefore >= 0);
    report.check("and one for the second mount's node: " + std::to_string(secondBefore),
                 secondBefore >= 0);
    if (firstBefore < 0 || secondBefore < 0)
    {
        return;
    }

    bool bothMoved = false;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(SweepDeadlineSeconds);
    while (!bothMoved && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(SweepPollMillis));
        bothMoved = fsuser::tests::readSweepCount(nodes.first) > firstBefore &&
                    fsuser::tests::readSweepCount(nodes.second) > secondBefore;
    }
    report.check("both counts advance within the deadline, so both sweeps are running",
                 bothMoved);
}

// bootstrap_dump is root's, so this reports what it could not reach rather than failing on it. A
// slot nobody holds carries a zero token, which is what a release writes and a scan reads as free.
void checkBootstrapSlots(fsuser::tests::Report& report, const MountNodes_t& nodes)
{
    report.section("the bootstrap slot table, when it can be read");

    const auto slots = fsuser::tests::readBootstrapSlots(nodes.first);
    if (!slots)
    {
        report.note("bootstrap_dump is not readable by this account, so the table is unchecked");
        return;
    }

    report.check("bootstrap_dump has rows to read", !slots->empty());

    std::int32_t held = 0;
    std::int32_t stampless = 0;
    std::set<std::uint32_t> owned;
    std::int32_t mine = 0;
    for (const auto& slot : *slots)
    {
        if (slot.token == 0)
        {
            continue;
        }
        ++held;
        stampless += (slot.heartbeat == 0) ? 1 : 0;
        if (slot.mine)
        {
            ++mine;
            owned.insert(slot.node);
        }
    }

    report.check("at least one slot is held, since a mount is what this case is talking to",
                 held > 0);
    report.check("every held slot carries a stamp, so none was claimed and then never ticked",
                 stampless == 0);
    report.check("each mount marks a slot of its own: " + std::to_string(mine), mine >= 2);
    report.check("and no two of those name the same node", owned.size() == static_cast<std::size_t>(mine));
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
    checkPermInfoIsReadable(report);
    checkDelegInfoIsRootOnly(report);

    try
    {
        const auto name = caseName("sysfs");
        auto mount = fsuser::tests::Mount(mounts.first());
        checkRegionInfoFollowsTheRegion(report, mount, name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_sysfs region_info", failure);
    }

    const MountNodes_t nodes{fsuser::tests::Mount(mounts.first()).getNodeId(),
                             fsuser::tests::Mount(mounts.second()).getNodeId()};
    checkTheSweepCounterMoves(report, nodes);
    checkBootstrapSlots(report, nodes);

    return report.summarise("test_sysfs");
}
