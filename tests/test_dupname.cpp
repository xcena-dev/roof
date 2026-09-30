// SPDX-License-Identifier: Apache-2.0
//
// test_dupname -- two nodes reaching for one name at the same moment.
//
// Creating a name allocates a RAT slot and links an index bucket, which is a read of shared state
// followed by a write to it. Two nodes doing that at once can both find the bucket empty, so the
// exclusion that makes exactly one of them win is the turn and nothing in the kernel.
//
// Two processes, each with its own mount, meet at a barrier and then create the
// same name with O_EXCL. Exactly one has to come away with the file and the region has to hold one
// entry for that name, which is where a second slot would show up.
//
// Rounds, because the window is narrow: a single attempt that happened to serialise would look the
// same as an exclusion that works.
//
//   test_dupname <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>

#include "fs/errors.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

// How many times the two sides race. Enough that a run which serialised every time is unlikely, few
// enough that a failing round is still findable in the output.
constexpr std::int32_t Rounds = 8;

// What one side answers about its own attempt.
constexpr std::int32_t Created = 1;
constexpr std::int32_t RefusedAsTaken = 0;
constexpr std::int32_t RefusedOtherwise = 2;

using fsuser::tests::caseName;
using fsuser::tests::countRegionsNamed;

[[nodiscard]] std::string roundName(const std::string& stem, std::int32_t round)
{
    return stem + "_r" + std::to_string(round);
}

// Creates @name or reports why not. EEXIST is the losing side's answer and is not a failure: which of
// the two wins is the race, and that exactly one does is the claim.
[[nodiscard]] std::int32_t tryToCreate(fsuser::tests::Mount& mount, const std::string& name)
{
    try
    {
        auto file = mount.open(name, O_CREAT | O_RDWR | O_EXCL, 0644);
        return Created;
    }
    catch (const fsuser::FsCodedError& refusal)
    {
        return refusal.code() == std::errc::file_exists ? RefusedAsTaken : RefusedOtherwise;
    }
    catch (const std::exception&)
    {
        return RefusedOtherwise;
    }
}

// The second node. It keeps one mount across every round, since attaching is not what is being
// raced, and it removes its own file when it won: the owner of a name is the process that created it
// and the other side cannot unlink it.
[[noreturn]] void raceAsSecondNode(fsuser::tests::Baton& begin, fsuser::tests::Baton& back,
                                   fsuser::tests::Mount& mount, const std::string& stem)
{
    try
    {
        for (std::int32_t round = 0; round < Rounds; ++round)
        {
            const std::string name = roundName(stem, round);

            static_cast<void>(begin.take());
            const std::int32_t mine = tryToCreate(mount, name);
            back.pass(mine);

            static_cast<void>(begin.take());
            if (mine == Created)
            {
                try
                {
                    mount.unlink(name);
                }
                catch (const std::exception&)
                {
                    back.pass(errno);
                    continue;
                }
            }
            back.pass(0);
        }
    }
    catch (const std::exception&)
    {
        back.pass(-1);
    }
    ::_exit(0);
}

void checkExactlyOneWins(fsuser::tests::Report& report, fsuser::tests::Baton& begin,
                         fsuser::tests::Baton& back, fsuser::tests::Mount& mount,
                         const std::string& stem)
{
    report.section("both nodes reach for the same name");

    std::int32_t bothWon = 0;
    std::int32_t neitherWon = 0;
    std::int32_t oddRefusals = 0;
    std::int32_t extraEntries = 0;
    std::int32_t roundsSeen = 0;

    for (std::int32_t round = 0; round < Rounds; ++round)
    {
        const std::string name = roundName(stem, round);

        begin.pass(1);
        const std::int32_t mine = tryToCreate(mount, name);
        const std::int32_t theirs = back.take();
        if (theirs != Created && theirs != RefusedAsTaken && theirs != RefusedOtherwise)
        {
            break;
        }
        ++roundsSeen;

        const std::int32_t winners = (mine == Created ? 1 : 0) + (theirs == Created ? 1 : 0);
        bothWon += (winners > 1) ? 1 : 0;
        neitherWon += (winners == 0) ? 1 : 0;
        oddRefusals +=
            ((mine == RefusedOtherwise) ? 1 : 0) + ((theirs == RefusedOtherwise) ? 1 : 0);

        const auto entries = countRegionsNamed(name);
        extraEntries += (entries > 1) ? 1 : 0;

        begin.pass(1);
        if (mine == Created)
        {
            try
            {
                mount.unlink(name);
            }
            catch (const std::exception& failure)
            {
                report.raised("removing this node's file", failure);
            }
        }
        static_cast<void>(back.take());
    }

    report.check("every round ran: " + std::to_string(roundsSeen) + " of " +
                     std::to_string(Rounds),
                 roundsSeen == Rounds);
    report.check("no round let both nodes create the name: " + std::to_string(bothWon) + " did",
                 bothWon == 0);
    report.check("no round left both nodes empty-handed: " + std::to_string(neitherWon) + " did",
                 neitherWon == 0);
    report.check("every refusal was EEXIST: " + std::to_string(oddRefusals) + " were not",
                 oddRefusals == 0);
    report.check("no name took a second region entry: " + std::to_string(extraEntries) + " did",
                 extraEntries == 0);
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
    const auto stem = caseName("dupname");

    fsuser::tests::Baton begin;
    fsuser::tests::Baton back;
    if (!begin.isOpen() || !back.isOpen())
    {
        report.check("the handoff pipes open", false);
        return report.summarise("test_dupname");
    }

    const ::pid_t other = ::fork();
    if (other == 0)
    {
        auto mount = fsuser::tests::Mount(mounts.second());
        raceAsSecondNode(begin, back, mount, stem);
    }
    report.check("fork succeeds", other > 0);
    if (other <= 0)
    {
        return report.summarise("test_dupname");
    }

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkExactlyOneWins(report, begin, back, mount, stem);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_dupname setup", failure);
    }

    std::int32_t status = 0;
    const bool reaped = ::waitpid(other, &status, 0) == other;
    report.check("the second node exits rather than being signalled",
                 reaped && (WIFEXITED(status) != 0) && WEXITSTATUS(status) == 0);

    return report.summarise("test_dupname");
}
