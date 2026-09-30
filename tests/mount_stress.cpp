// SPDX-License-Identifier: Apache-2.0
//
// mount_stress -- every mount the host has, on the metadata domain at the same time.
//
// A mount is what a node is here, so this forks one process per mount and each attaches its own
// Mount, which is what joins the domain separately. create, ftruncate and unlink all take the turn,
// so N nodes looping over them puts that domain under contention rather than the page cache.
//
// Two phases. In the first every node has a region of its own, and what it wrote has to read back: a
// placement handed to two nodes at once shows up as the wrong byte. In the second every node maps one
// region, each writing its own slice, and every node has to see every other slice.
//
// Then nothing may be left behind, because a create that raced an unlink leaks a RAT slot, and
// region_info is where a row nobody owns becomes visible.
//
// It scales to the mounts it is handed. Two says whether the contention is handled at all, and the
// eight a full host provisions fills the slot table while it does.
//
//   mount_stress <mount> [<mount2> ...]

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

// One cacheline, which is what a node stamps. In the first phase it is each end of the placement,
// where a neighbouring extent would run in; in the second it is one node's slice of a shared region.
constexpr std::uint64_t StampBytes = 64;

// Rounds per node in the first phase. Each round takes the turn three times, so this is what decides
// how long the domain stays contended.
constexpr std::int32_t Rounds = 64;

// The round number the shared phase stamps with, so its bytes cannot be mistaken for the first
// phase's.
constexpr std::int32_t SharedRound = Rounds;

// How long a node waits to see every other node's slice, and how often it looks. The write it is
// waiting for is one uncached store, so the wait is for the peer to be scheduled and not for a flush.
constexpr std::int32_t SharedDeadlineMillis = 10000;
constexpr std::int32_t SharedPollMillis = 2;

using fsuser::tests::Baton;
using fsuser::tests::caseName;
using fsuser::tests::listNames;
using fsuser::tests::Mount;
using fsuser::tests::placeFile;
using fsuser::tests::PlacementSize;
using fsuser::tests::readRegionRows;

// The byte a node stamps. Derived from both the node and the round, so a mapping holding another
// node's write says which node wrote it.
[[nodiscard]] std::uint8_t stampByte(std::int32_t node, std::int32_t round)
{
    return static_cast<std::uint8_t>(((node * 31) + round) & 0xFF);
}

[[nodiscard]] std::string roundName(const std::string& stem, std::int32_t node, std::int32_t round)
{
    return stem + "_n" + std::to_string(node) + "_r" + std::to_string(round);
}

// Whether @count bytes from @bytes all hold @stamp.
[[nodiscard]] bool sliceHolds(const std::uint8_t* bytes, std::uint8_t stamp, std::uint64_t count)
{
    for (std::uint64_t index = 0; index < count; ++index)
    {
        if (bytes[index] != stamp)
        {
            return false;
        }
    }
    return true;
}

// Reads back both stamped ends of a node's own region. False when a byte is not what this node
// wrote, which is what a second placement over the same extent produces.
[[nodiscard]] bool stampHeld(const std::uint8_t* bytes, std::uint8_t stamp, std::int32_t node,
                             std::int32_t round)
{
    if (sliceHolds(bytes, stamp, StampBytes) &&
        sliceHolds(bytes + PlacementSize - StampBytes, stamp, StampBytes))
    {
        return true;
    }
    std::fprintf(stderr, "  FAIL node %d round %d: an end of the placement is not 0x%02x\n", node,
                 round, stamp);
    return false;
}

// One node's first phase, in its own process. Returns 0 when every round completed and read back
// what it wrote, so the parent reads this as the node's verdict.
[[nodiscard]] std::int32_t runOwnRegions(const std::string& point, const std::string& stem,
                                         std::int32_t node)
{
    try
    {
        auto mount = Mount(point);

        for (std::int32_t round = 0; round < Rounds; ++round)
        {
            const auto name = roundName(stem, node, round);
            const auto stamp = stampByte(node, round);

            // Closed before the unlink below, so the name goes away with nothing still holding it.
            // O_EXCL, and the whole placement in one resize: a name or an extent this round did not
            // make itself is the collision the round is here to catch.
            {
                auto file = placeFile(mount, name, O_EXCL);

                auto mapping = file.map(PlacementSize, PROT_READ | PROT_WRITE);
                auto* bytes = static_cast<std::uint8_t*>(mapping.get());

                std::memset(bytes, stamp, StampBytes);
                std::memset(bytes + PlacementSize - StampBytes, stamp, StampBytes);

                if (!stampHeld(bytes, stamp, node, round))
                {
                    return 1;
                }
            }

            mount.unlink(name);
        }
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "  FAIL node %d: %s\n", node, failure.what());
        return 1;
    }
    return 0;
}

// Waits for every node's slice of @bytes to hold its stamp. Polls rather than meeting the others at
// a barrier, so a node that died takes the deadline with it instead of blocking the rest forever.
[[nodiscard]] bool everySliceArrived(const std::uint8_t* bytes, std::int32_t nodes, std::int32_t node)
{
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(SharedDeadlineMillis);

    for (std::int32_t other = 0; other < nodes; ++other)
    {
        const auto stamp = stampByte(other, SharedRound);
        const auto* slice = bytes + (static_cast<std::uint64_t>(other) * StampBytes);

        while (!sliceHolds(slice, stamp, StampBytes))
        {
            if (std::chrono::steady_clock::now() > until)
            {
                std::fprintf(stderr, "  FAIL node %d: node %d's slice reads 0x%02x, not 0x%02x\n",
                             node, other, slice[0], stamp);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SharedPollMillis));
        }
    }
    return true;
}

// One node's second phase: map the region every other node is also mapping, stamp this node's slice,
// and then read all of them. Returns 0 when every slice arrived.
[[nodiscard]] std::int32_t runSharedRegion(const std::string& point, const std::string& name,
                                           std::int32_t node, std::int32_t nodes)
{
    try
    {
        auto mount = Mount(point);
        auto file = mount.open(name, O_RDWR);
        auto mapping = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        auto* bytes = static_cast<std::uint8_t*>(mapping.get());

        auto* mine = bytes + (static_cast<std::uint64_t>(node) * StampBytes);
        std::memset(mine, stampByte(node, SharedRound), StampBytes);

        return everySliceArrived(bytes, nodes, node) ? 0 : 1;
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "  FAIL node %d shared: %s\n", node, failure.what());
        return 1;
    }
}

// How many region_info rows carry a name this run created. A row left here after every node
// unlinked its own names is a leaked slot.
[[nodiscard]] std::int32_t rowsLeftNamed(const std::string& stem)
{
    std::int32_t found = 0;
    for (const auto& row : readRegionRows())
    {
        if (row.name.rfind(stem, 0) == 0)
        {
            ++found;
        }
    }
    return found;
}

// Releases every child of a phase at once and then waits for all of them. Returns how many exited 0.
[[nodiscard]] std::int32_t releaseAndWait(Baton& gate, const std::vector<::pid_t>& children)
{
    for (const auto released : children)
    {
        gate.pass(static_cast<std::int32_t>(released));
    }

    std::int32_t finished = 0;
    for (const auto child : children)
    {
        std::int32_t status = 0;
        if (::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            ++finished;
        }
    }
    return finished;
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 1;
    }

    fsuser::tests::Report report;
    const auto stem = caseName("stress");
    const auto nodes = static_cast<std::int32_t>(mounts.points.size());

    // Every child blocks on this until the parent has forked all of them, so the work overlaps
    // rather than running in the order the children were started.
    Baton gate;
    if (!gate.isOpen())
    {
        report.note("no pipe for the start gate, so nothing was contended");
        return fsuser::tests::SkipStatus;
    }

    report.section("a region per node, created and unlinked in a loop");
    report.note("nodes: " + std::to_string(nodes) +
                ", rounds each: " + std::to_string(Rounds));

    std::vector<::pid_t> children;
    std::int32_t node = 0;
    for (const auto& point : mounts.points)
    {
        const auto child = ::fork();
        if (child == 0)
        {
            static_cast<void>(gate.take());
            ::_exit(runOwnRegions(point, stem, node));
        }
        if (child < 0)
        {
            report.check("forked a process for every mount", false);
            break;
        }
        children.emplace_back(child);
        ++node;
    }

    const auto ownDone = releaseAndWait(gate, children);
    report.check("every node finished all of its rounds: " + std::to_string(ownDone) + " of " +
                     std::to_string(nodes),
                 ownDone == nodes);

    report.section("one region, every node mapping it at once");

    const auto shared = stem + "_shared";
    children.clear();
    node = 0;

    report.checkAccepted("the region every node will map is placed", [&]
                         {
                             auto mount = Mount(mounts.first());
                             auto file = placeFile(mount, shared, O_EXCL);
                         });

    for (const auto& point : mounts.points)
    {
        const auto child = ::fork();
        if (child == 0)
        {
            static_cast<void>(gate.take());
            ::_exit(runSharedRegion(point, shared, node, nodes));
        }
        if (child < 0)
        {
            report.check("forked a process for every mount", false);
            break;
        }
        children.emplace_back(child);
        ++node;
    }

    const auto sharedDone = releaseAndWait(gate, children);
    report.check("every node saw every other node's slice: " + std::to_string(sharedDone) + " of " +
                     std::to_string(nodes),
                 sharedDone == nodes);

    report.checkAccepted("and the node that placed it can unlink it", [&]
                         {
                             auto mount = Mount(mounts.first());
                             mount.unlink(shared);
                         });

    report.section("what the run left behind");

    std::int32_t clean = 0;
    for (const auto& point : mounts.points)
    {
        if (listNames(point, stem).empty())
        {
            ++clean;
        }
    }
    report.check("no name this run created is left in any mount", clean == nodes);

    const auto leaked = rowsLeftNamed(stem);
    report.check("and region_info holds no row for one: " + std::to_string(leaked) + " left",
                 leaked == 0);

    return report.summarise("mount_stress");
}
