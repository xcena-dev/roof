// SPDX-License-Identifier: Apache-2.0
//
// test_meta_lock -- which paths take the cross-node half of the metadata lock, and which do not.
//
// The kernel excludes nodes itself: a metadata writer takes its mount's mutex and then a turn on the
// shared lock region through that mount's helper. Only the first half is visible from inside one
// node, so a case that watched effects alone could not tell a path that asked for the turn from one
// that never did. node<N>/test/meta_lock is what makes the second half countable.
//
// Every claim here is a delta across one operation. GC asks for the same turn on its sweeps, so it
// can only add: a must-take claim is therefore "at least one" and a must-not-take claim runs the
// operation many times and allows the few a sweep could contribute.
//
// The last arm is the one that cannot be reached from a single mount. A peer with no delegation is
// refused on the record, which sends the kernel to its helper, and the row that answer earns is
// written under the turn.
//
//   test_meta_lock <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::MetaTurns_t;
using fsuser::tests::PlacementSize;
using fsuser::tests::readMetaTurns;

// How many times a lookup runs before its total is judged. Without the turn a lookup would add this
// many, so a handful from a concurrent sweep cannot be mistaken for the path taking it.
constexpr std::int32_t LookupRounds = 200;
constexpr std::uint64_t SweepAllowance = 4;

[[nodiscard]] std::uint64_t turnsTaken(const fsuser::tests::Mount& mount)
{
    const auto turns = readMetaTurns(mount.getNodeId());
    return turns ? turns->taken : 0;
}

// A create places the file too, and each of those is one turn, so this is the arm that shows the
// count moving with the number of metadata writes rather than with the number of calls.
void checkCreateAndPlaceTakeTurns(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                  const std::string& name)
{
    report.section("a create and its placement each take a turn");

    const auto before = turnsTaken(mount);
    auto file = fsuser::tests::placeFile(mount, name);
    const auto after = turnsTaken(mount);

    report.check("the file opened", file.isOpen());
    report.check("creating and placing took at least two turns", after - before >= 2);
    report.note("turns taken: " + std::to_string(after - before));
}

void checkAMapWithARowTakesNothing(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                   const std::string& name)
{
    report.section("the owner's own mapping asks for no turn");

    auto file = mount.open(name, O_RDWR);
    const auto before = turnsTaken(mount);
    auto mapped = file.map(PlacementSize, PROT_READ | PROT_WRITE);
    const auto after = turnsTaken(mount);

    report.check("the owner maps", mapped.isMapped());
    // The owner is already permitted on the record, so no delegation is written and the lock has
    // nothing to protect.
    report.check("the mapping took no turn of its own", after - before <= SweepAllowance);
}

void checkALookupTakesNothing(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                              const std::string& name)
{
    report.section("a lookup asks for no turn");

    std::int32_t opened = 0;
    const auto before = turnsTaken(mount);

    for (std::int32_t round = 0; round < LookupRounds; ++round)
    {
        auto file = mount.open(name, O_RDONLY);
        opened += file.isOpen() ? 1 : 0;
    }
    const auto after = turnsTaken(mount);

    report.check("every lookup opened", opened == LookupRounds);
    report.check(std::to_string(LookupRounds) + " lookups took no turn between them",
                 after - before <= SweepAllowance);
    report.note("turns taken: " + std::to_string(after - before));
}

// A child, so the pid the record was stamped with is not the one asking. An owner reaching its own
// region is admitted without a row, and it is the absence of a row that sends the kernel upcalling.
[[nodiscard]] bool mapAsAStranger(fsuser::tests::Mount& peer, const std::string& name)
{
    const ::pid_t child = ::fork();
    if (child < 0)
    {
        return false;
    }
    if (child == 0)
    {
        try
        {
            auto file = peer.open(name, O_RDWR);
            auto mapped = file.map(PlacementSize, PROT_READ);
            ::_exit(mapped.isMapped() ? 0 : 1);
        }
        catch (const std::exception&)
        {
            ::_exit(1);
        }
    }

    std::int32_t status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void checkAnUnpermittedMapTakesATurn(fsuser::tests::Report& report, fsuser::tests::Mount& peer,
                                     const std::string& name)
{
    report.section("a map with no row upcalls, and the row it earns is written under a turn");

    const auto before = turnsTaken(peer);
    const bool mapped = mapAsAStranger(peer, name);
    const auto after = turnsTaken(peer);

    report.check("the helper let the stranger map", mapped);
    report.check("the row it earned was written under a turn", after - before >= 1);
    report.note("turns taken on the peer node: " + std::to_string(after - before));
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
    const auto name = caseName("meta_lock");
    auto owner = fsuser::tests::Mount(mounts.first());
    auto peer = fsuser::tests::Mount(mounts.second());

    const auto ownerTurns = readMetaTurns(owner.getNodeId());
    const auto peerTurns = readMetaTurns(peer.getNodeId());
    if (!ownerTurns || !peerTurns)
    {
        report.note("no test/meta_lock under both mounts, so no turn is countable");
        return fsuser::tests::SkipStatus;
    }
    if (peerTurns->refused != 0 && peerTurns->taken == 0)
    {
        report.note("this mount has no helper serving locks, so every turn was refused");
        return fsuser::tests::SkipStatus;
    }

    try
    {
        checkCreateAndPlaceTakeTurns(report, owner, name);
        checkAMapWithARowTakesNothing(report, owner, name);
        checkALookupTakesNothing(report, owner, name);
        checkAnUnpermittedMapTakesATurn(report, peer, name);

        report.section("an unlink takes a turn");
        const auto before = turnsTaken(owner);
        owner.unlink(name);
        report.check("removing the name took a turn", turnsTaken(owner) - before >= 1);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_meta_lock setup", failure);
    }

    return report.summarise("test_meta_lock");
}
