// SPDX-License-Identifier: Apache-2.0
//
// test_security_deleg_revoke -- whether an owner can take back what an upcall granted.
//
// A helper's answer is written down as a row naming the process it answered about, and that row
// carries the account the process runs as. The revoke verb names an account, so one call has to
// reach the account's own row and the upcall-written rows of its processes alike.
//
// The upcall count is the observation, because a mapping a row already covers asks nobody while
// one with no row earns its row by asking. A GC sweep takes metadata turns but asks nothing, so
// this count needs no allowance for one.
//
//   test_security_deleg_revoke <mount-a>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::Handoff_t;
using fsuser::tests::PlacementSize;
using fsuser::tests::readDaemonAsked;

// An account nobody in this run holds. Its row is on the region to be counted, not to be used: what
// it answers is whether the sweep stopped at the account it was given.
constexpr std::uint32_t BystanderUid = 62150;

[[nodiscard]] std::uint64_t countUpcalls(const fsuser::tests::Mount& mount)
{
    const auto asked = readDaemonAsked(mount.getNodeId());
    return asked ? *asked : 0;
}

// The child maps three times, waiting for the parent between the second and the third so that the
// revoke lands in between. Each result goes back as an errno, and 0 is a mapping that landed.
[[noreturn]] void runConsumer(std::int32_t inherited, Handoff_t& handoff)
{
    handoff.ack.pass(fsuser::tests::probeReadMap(inherited, PlacementSize));
    handoff.ack.pass(fsuser::tests::probeReadMap(inherited, PlacementSize));
    static_cast<void>(handoff.begin.take());
    handoff.ack.pass(fsuser::tests::probeReadMap(inherited, PlacementSize));
    ::_exit(0);
}

void checkRevokeReachesTheUpcallRow(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("an owner taking back what the helper granted");

    const auto name = caseName("security_deleg_revoke");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        // Written before the revoke and read after it, so a sweep that took more than the account
        // it was handed is seen rather than assumed.
        file.grantPermission(BystanderUid, fsuser::AnyId, fsuser::Permission::Read);

        Handoff_t handoff;
        if (!report.anyFailed() && !handoff.isOpen())
        {
            report.check("the parent and child can talk", false);
            mount.unlink(name);
            return;
        }

        const auto beforeFirst = countUpcalls(mount);
        const ::pid_t child = ::fork();
        if (child == 0)
        {
            runConsumer(file.get(), handoff);
        }
        report.check("fork succeeds", child > 0);
        if (child <= 0)
        {
            mount.unlink(name);
            return;
        }

        const std::int32_t firstMap = handoff.ack.take();
        report.checkErrno("the helper grants the child its first mapping", firstMap == 0, firstMap);
        const auto afterFirst = countUpcalls(mount);
        report.check("that grant cost an upcall", afterFirst > beforeFirst);

        const std::int32_t secondMap = handoff.ack.take();
        report.checkErrno("the child maps again", secondMap == 0, secondMap);
        report.check("the second mapping rides the row and asks nobody",
                     countUpcalls(mount) == afterFirst);

        // The verb names an account, and the row the helper wrote names a process of that same
        // account. One call has to be enough for both.
        report.checkAccepted("the owner takes that account's grant back",
                             [&]
                             {
                                 file.revokePermission(::getuid(), fsuser::AnyId);
                             });

        // ENOENT before the child is let go: nothing of that account is left, which is what says
        // the call above did not stop at the first row it matched.
        report.checkRefusedWith("nothing of that account is left", std::errc::no_such_file_or_directory,
                                [&]
                                {
                                    file.revokePermission(::getuid(), fsuser::AnyId);
                                });

        report.checkAccepted("another account's row was left where it was",
                             [&]
                             {
                                 file.revokePermission(BystanderUid, fsuser::AnyId);
                             });

        const auto beforeThird = countUpcalls(mount);
        handoff.begin.pass(1);
        const std::int32_t thirdMap = handoff.ack.take();
        std::int32_t status = 0;
        static_cast<void>(::waitpid(child, &status, 0));

        // Whether the third mapping lands is the policy's answer, not this case's question. What
        // says the row is gone is that the helper was asked at all.
        report.check("the revoke reaches the row the helper wrote",
                     countUpcalls(mount) > beforeThird);
        report.note("the third mapping answered errno " + std::to_string(thirdMap));
    }
    catch (const std::exception& failure)
    {
        report.raised("security deleg revoke", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    // The account the revoke names is this process's real uid, and it is also the one the kernel
    // attests for the child. Dropping here keeps the row and the verb naming the same account.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkRevokeReachesTheUpcallRow(report, mount);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_deleg_revoke", failure);
    }

    return report.summarise("test_security_deleg_revoke");
}
