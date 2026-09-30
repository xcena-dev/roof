// SPDX-License-Identifier: Apache-2.0
//
// test_unlink_race -- two nodes creating, mapping and unlinking at once read only their own bytes.
//
// unlink and open order their two writes so that whichever came second sees the other: open sets
// its reference bit and then reads the name, unlink clears the name and then reads the bits. This
// case runs the two against each other for a while and asks the only question that matters to a
// reader: did any mapping ever show bytes that were not its own file's. It ends by waiting for the
// sweeps and checking that nothing the run made is left standing.
//
// Two arms. In the first each node works its own names, so a stray read would be the other node's
// pattern. In the second one node creates, fills and unlinks a single name in a loop while the other
// opens it whenever it can: a file it manages to map must hold one consistent pattern, whose seed is
// its own first word, and never a torn or foreign one.
//
//   test_unlink_race <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/listing.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::Baton;
using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;
using fsuser::tests::readSweepCount;
using fsuser::tests::waitForSweeps;

constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::int32_t RaceSeconds = 10;

// The first word is the seed itself, so a reader recovers the seed from the bytes and needs no
// side channel to judge the rest.
constexpr std::uint32_t OwnSeedBase = 0x0A000000U;
constexpr std::uint32_t SharedSeedBase = 0x0B000000U;

[[nodiscard]] bool holdsOwnPattern(const volatile std::uint32_t* words)
{
    const std::uint32_t seed = words[0];
    for (std::uint64_t index = 1; index < PlacedWords; ++index)
    {
        if (words[index] != fsuser::tests::patternWord(seed, index))
        {
            return false;
        }
    }
    return true;
}

// Clears the seed word first, fills words 1.., and writes the seed last. A freed extent keeps its
// bytes, so a remade file starts with the previous seed in place; the clear is what stops a reader
// taking that seed for a whole pattern while the body is still being written.
void fillSeedLast(volatile std::uint32_t* words, std::uint32_t seed)
{
    words[0] = 0;
    for (std::uint64_t index = 1; index < PlacedWords; ++index)
    {
        words[index] = fsuser::tests::patternWord(seed, index);
    }
    words[0] = seed;
}

// The seed read before and after the body: the same non-zero value both times means no clear ran
// during the scan, and the body therefore belongs to that seed.
[[nodiscard]] bool holdsSteadyPattern(const volatile std::uint32_t* words, std::uint32_t* seedOut)
{
    const std::uint32_t before = words[0];
    const bool matched = holdsOwnPattern(words);
    const std::uint32_t after = words[0];
    *seedOut = before;
    return before == after && matched;
}

struct Tally_t
{
    std::int32_t rounds{0};
    std::int32_t wrong{0};
    std::int32_t skipped{0};
};

[[nodiscard]] bool timeIsUp(std::chrono::steady_clock::time_point until)
{
    return std::chrono::steady_clock::now() >= until;
}

// One node's loop over its own names: make, fill, read back, unlink, again.
[[nodiscard]] Tally_t churnOwnNames(fsuser::tests::Mount& mount, const std::string& stem, std::uint32_t seedBase)
{
    Tally_t tally;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(RaceSeconds);
    while (!timeIsUp(until))
    {
        const auto name = stem + std::to_string(tally.rounds);
        const std::uint32_t seed = seedBase + static_cast<std::uint32_t>(tally.rounds);
        try
        {
            auto file = fsuser::tests::placeFile(mount, name, O_EXCL);
            {
                auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
                fillSeedLast(static_cast<std::uint32_t*>(stored.get()), seed);
                if (static_cast<const std::uint32_t*>(stored.get())[0] != seed ||
                    !holdsOwnPattern(static_cast<const std::uint32_t*>(stored.get())))
                {
                    ++tally.wrong;
                }
            }
            mount.unlink(name);
        }
        catch (const fsuser::FsCodedError&)
        {
            // A slot whose last holder could not take the turn waits for the sweep, so a create
            // can still find the table full. That is a refusal, not a foreign byte.
            ++tally.skipped;
        }
        catch (const std::exception&)
        {
            ++tally.wrong;
        }
        ++tally.rounds;
    }
    return tally;
}

// The maker's half of the shared name: create, fill, unlink, as fast as it goes. ENOSPC or EEXIST
// from a slot the reader still holds is the race working, not a fault.
[[nodiscard]] Tally_t churnSharedName(fsuser::tests::Mount& mount, const std::string& name)
{
    Tally_t tally;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(RaceSeconds);
    while (!timeIsUp(until))
    {
        const std::uint32_t seed = SharedSeedBase + static_cast<std::uint32_t>(tally.rounds);
        try
        {
            auto file = fsuser::tests::placeFile(mount, name);
            {
                auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
                fillSeedLast(static_cast<std::uint32_t*>(stored.get()), seed);
            }
            mount.unlink(name);
        }
        catch (const fsuser::FsCodedError&)
        {
            ++tally.skipped;
        }
        catch (const std::exception&)
        {
            ++tally.wrong;
        }
        ++tally.rounds;
    }
    return tally;
}

// The reader's half: open whenever the name is there, map, and judge the bytes. A name that is gone
// or a file not yet filled is skipped; a mapping that holds a torn or foreign pattern is wrong.
[[nodiscard]] Tally_t readSharedName(fsuser::tests::Mount& mount, const std::string& name)
{
    Tally_t tally;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(RaceSeconds);
    while (!timeIsUp(until))
    {
        try
        {
            auto file = mount.open(name, O_RDONLY);
            auto seen = file.map(PlacementSize, PROT_READ);
            const auto* words = static_cast<const volatile std::uint32_t*>(seen.get());
            const std::uint32_t before = words[0];
            std::uint32_t seed = 0;
            const bool steady = holdsSteadyPattern(words, &seed);
            // A seed that moved during the scan is the maker at work on a remade file, which the
            // reader cannot judge; a seed that held still over a body that does not match it is wrong.
            if ((before & 0xFF000000U) == SharedSeedBase && !steady && words[0] == before)
            {
                ++tally.wrong;
            }
            ++tally.rounds;
        }
        catch (const fsuser::FsCodedError&)
        {
            // Gone between the lookup and the open, or not yet placed: both are the race.
            ++tally.skipped;
        }
        catch (const std::exception&)
        {
            ++tally.wrong;
        }
    }
    return tally;
}

// Runs @work in a forked child and hands its tally back through @answers, one value per field.
template <typename T_Work>
[[nodiscard]] ::pid_t runInChild(Baton& answers, T_Work&& work)
{
    const ::pid_t child = ::fork();
    if (child == 0)
    {
        Tally_t tally;
        try
        {
            tally = work();
        }
        catch (const std::exception&)
        {
            tally.wrong = -1;
        }
        answers.pass(tally.rounds);
        answers.pass(tally.wrong);
        answers.pass(tally.skipped);
        ::_exit(0);
    }
    return child;
}

[[nodiscard]] Tally_t collect(Baton& answers, ::pid_t child)
{
    Tally_t tally;
    tally.rounds = answers.take();
    tally.wrong = answers.take();
    tally.skipped = answers.take();
    std::int32_t status = 0;
    ::waitpid(child, &status, 0);
    return tally;
}

void reportTally(fsuser::tests::Report& report, const std::string& who, const Tally_t& tally)
{
    report.check(who + " did " + std::to_string(tally.rounds) + " rounds", tally.rounds > 0);
    report.note(who + " skipped " + std::to_string(tally.skipped) + " refusals the race produced");
    report.check(who + " read no bytes that were not its own file's (" + std::to_string(tally.wrong) + " wrong)",
                 tally.wrong == 0);
}

void checkOwnNamesRace(fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts, const std::string& stem)
{
    report.section("each node churns its own names");

    Baton fromFirst;
    Baton fromSecond;
    const auto first = runInChild(fromFirst,
                                  [&]
                                  {
                                      fsuser::tests::Mount mount{mounts.first()};
                                      return churnOwnNames(mount, stem + "a", OwnSeedBase);
                                  });
    const auto second = runInChild(fromSecond,
                                   [&]
                                   {
                                       fsuser::tests::Mount mount{mounts.second()};
                                       return churnOwnNames(mount, stem + "b", OwnSeedBase + 0x00800000U);
                                   });
    report.check("both nodes start", first > 0 && second > 0);
    reportTally(report, "node a", collect(fromFirst, first));
    reportTally(report, "node b", collect(fromSecond, second));
}

void checkSharedNameRace(fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts, const std::string& name)
{
    report.section("one node remakes a name while the other reads it");

    Baton fromMaker;
    Baton fromReader;
    const auto maker = runInChild(fromMaker,
                                  [&]
                                  {
                                      fsuser::tests::Mount mount{mounts.first()};
                                      return churnSharedName(mount, name);
                                  });
    const auto reader = runInChild(fromReader,
                                   [&]
                                   {
                                       fsuser::tests::Mount mount{mounts.second()};
                                       return readSharedName(mount, name);
                                   });
    report.check("both sides start", maker > 0 && reader > 0);
    reportTally(report, "the maker", collect(fromMaker, maker));
    reportTally(report, "the reader", collect(fromReader, reader));
}

void checkNothingLeft(fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts, const std::string& prefix)
{
    report.section("what the run leaves behind");

    const auto nodeA = fsuser::tests::Mount(mounts.first()).getNodeId();
    const auto nodeB = fsuser::tests::Mount(mounts.second()).getNodeId();
    const auto beforeA = readSweepCount(nodeA);
    const auto beforeB = readSweepCount(nodeB);
    report.check("both nodes sweep twice",
                 beforeA >= 0 && beforeB >= 0 && waitForSweeps(nodeA, beforeA) && waitForSweeps(nodeB, beforeB));
    report.check("no name of this run is left on the mount", fsuser::tests::listNames(mounts.first(), prefix).empty());
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto prefix = caseName("race");
        checkOwnNamesRace(report, mounts, prefix + "_own_");
        checkSharedNameRace(report, mounts, prefix + "_shared");
        checkNothingLeft(report, mounts, prefix);
    };
    return fsuser::tests::runCase(argc, argv, "test_unlink_race", 2, body);
}
