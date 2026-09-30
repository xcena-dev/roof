// SPDX-License-Identifier: Apache-2.0
//
// test_fork_inherit -- a descriptor survives fork and the owner's standing does not.
//
// Ownership is scoped to the tgid, so a forked child holding the parent's descriptor is a different
// principal. The parent reaches its own file on the record alone and asks nobody. The child has no
// record of its own, so what it may map is the helper's to answer, and the row that answer earns is
// written under a metadata turn.
//
// The turn is what makes the two tellable apart: a mapping that rides the owner's standing writes
// nothing, and one the helper granted writes a row.
//
//   test_fork_inherit <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <string>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::MetaTurns_t;
using fsuser::tests::PlacementSize;
using fsuser::tests::readMetaTurns;

// A concurrent GC sweep takes turns of its own, so a count that should not move is allowed this
// many. The same allowance the meta_lock case uses.
constexpr std::uint64_t SweepAllowance = 4;

[[nodiscard]] std::uint64_t turnsTaken(const fsuser::tests::Mount& mount)
{
    const auto turns = readMetaTurns(mount.getNodeId());
    return turns ? turns->taken : 0;
}

// A mapping cannot cross back to the parent, so the child exits with the errno it was given. No
// Linux errno reaches 256, and a zero status is the one outcome with no errno: the mapping landed.
[[noreturn]] void mapAndExit(std::int32_t inherited)
{
    ::_exit(fsuser::tests::probeReadMap(inherited, PlacementSize));
}

void checkInheritedDescriptor(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("a forked child holding the owner's descriptor");

    const auto name = caseName("fork_inherit");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        {
            const auto before = turnsTaken(mount);
            auto owned = file.map(PlacementSize, PROT_READ | PROT_WRITE);
            const auto after = turnsTaken(mount);

            report.check("the owner maps its own file", owned.isMapped());
            report.check("the owner's mapping writes no row", after - before <= SweepAllowance);
        }

        const auto before = turnsTaken(mount);
        const ::pid_t child = ::fork();
        if (child == 0)
        {
            mapAndExit(file.get());
        }
        report.check("fork succeeds", child > 0);

        if (child > 0)
        {
            std::int32_t status = 0;
            const bool reaped = ::waitpid(child, &status, 0) == child;
            report.check("the child exits rather than being signalled",
                         reaped && (WIFEXITED(status) != 0));
            if (reaped && (WIFEXITED(status) != 0))
            {
                const std::int32_t left = WEXITSTATUS(status);
                report.checkErrno("the helper decides for the child, and admits it", left == 0,
                                  left);
            }
        }
        const auto after = turnsTaken(mount);

        // The child is not the owner, so nothing on the record covered it and the grant it was
        // given had to be written down.
        report.check("the child's mapping earns a row of its own", after > before);
        report.note("turns taken across the fork: " + std::to_string(after - before));

        const auto reopened = turnsTaken(mount);
        auto again = file.map(PlacementSize, PROT_READ);
        report.check("the owner still maps after the child earned its row", again.isMapped());
        report.check("and still writes no row of its own",
                     turnsTaken(mount) - reopened <= SweepAllowance);
    }
    catch (const std::exception& failure)
    {
        report.raised("fork inherit", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkInheritedDescriptor(report, mount);
    };
    return fsuser::tests::runCase(argc, argv, "test_fork_inherit", 1, body);
}
