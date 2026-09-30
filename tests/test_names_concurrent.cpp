// SPDX-License-Identifier: Apache-2.0
//
// test_names_concurrent -- two nodes filling and emptying the same namespace at once.
//
// Linking a name reads the index and then writes it, and the kernel excludes only this node's own
// threads. So the exclusion that keeps two nodes from losing each other's entries is the metadata
// turn, and what this case asks is whether every name survives a round where both nodes are inside
// it the whole time.
//
// Two processes, each with its own mount, and each holding its files open for the
// whole round. Holding them open is what makes the count trustworthy: a name's owner is the process
// that created it, and the GC reclaims a name whose owner has gone, so a node that created and exited
// would have its entries swept while the other side was still counting.
//
//   test_names_concurrent <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"

namespace
{

// How many names each node links per round. Large enough that the two nodes overlap inside one
// round, small enough that two nodes together stay well inside the RAT.
constexpr std::int32_t PerNode = 40;

// How many create-and-empty rounds run. The window is the turn's, so one round that happened to
// serialise would look like exclusion that works.
constexpr std::int32_t Rounds = 4;

// What one side answers about its own half of a round.
constexpr std::int32_t Linked = 0;
constexpr std::int32_t Failed = 1;

using fsuser::tests::caseName;
using fsuser::tests::listNames;

[[nodiscard]] std::string roundPrefix(const std::string& stem, const char* tag, std::int32_t round)
{
    return stem + "_" + tag + "_r" + std::to_string(round) + "_";
}

// Links PerNode names under @prefix and hands back the descriptors, so the caller decides when the
// owner stops being alive for them.
[[nodiscard]] std::vector<fsuser::File> linkBatch(fsuser::tests::Mount& mount,
                                                  const std::string& prefix)
{
    std::vector<fsuser::File> held;
    held.reserve(static_cast<std::size_t>(PerNode));
    for (std::int32_t index = 0; index < PerNode; ++index)
    {
        held.push_back(mount.open(prefix + std::to_string(index), O_CREAT | O_RDWR, 0644));
    }
    return held;
}

void unlinkBatch(fsuser::tests::Mount& mount, const std::string& prefix)
{
    for (std::int32_t index = 0; index < PerNode; ++index)
    {
        mount.unlink(prefix + std::to_string(index));
    }
}

// The second node. Its half of each round is bracketed by the same two handoffs the first node uses,
// so both are inside the linking phase together and neither empties before the other has counted.
[[noreturn]] void runSecondNode(fsuser::tests::Handoff_t& handoff, const std::string& mount,
                                const std::string& stem)
{
    try
    {
        auto point = fsuser::tests::Mount(mount);
        for (std::int32_t round = 0; round < Rounds; ++round)
        {
            const std::string prefix = roundPrefix(stem, "b", round);

            static_cast<void>(handoff.begin.take());
            auto held = linkBatch(point, prefix);
            handoff.ack.pass(Linked);

            static_cast<void>(handoff.begin.take());
            held.clear();
            unlinkBatch(point, prefix);
            handoff.ack.pass(Linked);
        }
    }
    catch (const std::exception&)
    {
        handoff.ack.pass(Failed);
    }
    ::_exit(0);
}

void checkEveryNameSurvives(fsuser::tests::Report& report, fsuser::tests::Handoff_t& handoff,
                            fsuser::tests::Mount& mount, const fsuser::tests::Mounts_t& mounts,
                            const std::string& stem)
{
    report.section("both nodes linking, then both emptying");

    const std::string& mountA = mounts.first();
    const std::string& mountB = mounts.second();

    std::int32_t roundsSeen = 0;
    std::int32_t shortListings = 0;
    std::int32_t mismatchedMounts = 0;
    std::int32_t leftovers = 0;

    for (std::int32_t round = 0; round < Rounds; ++round)
    {
        const std::string mine = roundPrefix(stem, "a", round);
        const std::string theirs = roundPrefix(stem, "b", round);

        handoff.begin.pass(1);
        auto held = linkBatch(mount, mine);
        if (handoff.ack.take() != Linked)
        {
            break;
        }
        ++roundsSeen;

        const auto wanted = static_cast<std::size_t>(PerNode);
        const auto seenMineHere = listNames(mountA, mine);
        const auto seenTheirsHere = listNames(mountA, theirs);
        shortListings += (seenMineHere.size() == wanted && seenTheirsHere.size() == wanted) ? 0 : 1;
        mismatchedMounts += (seenMineHere == listNames(mountB, mine) &&
                             seenTheirsHere == listNames(mountB, theirs))
                                ? 0
                                : 1;

        handoff.begin.pass(1);
        held.clear();
        unlinkBatch(mount, mine);
        if (handoff.ack.take() != Linked)
        {
            break;
        }

        leftovers += (listNames(mountA, mine).empty() && listNames(mountA, theirs).empty()) ? 0 : 1;
    }

    report.check("every round ran: " + std::to_string(roundsSeen) + " of " +
                     std::to_string(Rounds),
                 roundsSeen == Rounds);
    report.check("every round listed both nodes' names in full: " +
                     std::to_string(shortListings) + " did not",
                 shortListings == 0);
    report.check("the two mounts agreed in every round: " + std::to_string(mismatchedMounts) +
                     " did not",
                 mismatchedMounts == 0);
    report.check("every round emptied completely: " + std::to_string(leftovers) + " did not",
                 leftovers == 0);
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
    const auto stem = caseName("names_conc");

    fsuser::tests::Handoff_t handoff;
    if (!handoff.isOpen())
    {
        report.check("the handoff pipes open", false);
        return report.summarise("test_names_concurrent");
    }

    const ::pid_t other = ::fork();
    if (other == 0)
    {
        runSecondNode(handoff, mounts.second(), stem);
    }
    report.check("fork succeeds", other > 0);
    if (other <= 0)
    {
        return report.summarise("test_names_concurrent");
    }

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkEveryNameSurvives(report, handoff, mount, mounts, stem);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_names_concurrent first node", failure);
    }

    std::int32_t status = 0;
    const bool reaped = ::waitpid(other, &status, 0) == other;
    report.check("the second node exits rather than being signalled",
                 reaped && (WIFEXITED(status) != 0) && WEXITSTATUS(status) == 0);

    return report.summarise("test_names_concurrent");
}
