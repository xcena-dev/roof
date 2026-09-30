// SPDX-License-Identifier: Apache-2.0
//
// test_security_gid_revoke -- whether a revoke naming a gid reaches a process row whose holder
// carries that group only as a supplementary one.
//
// A row records the one gid its holder ran under, so a supplementary group of that holder is written
// down nowhere. The row does carry the pid, and the groups that process holds now are what decide
// whether the revoke names it.
//
// The case needs an account in some group besides the one it runs under, which an ordinary login
// already is. A run whose account carries none reports a skip rather than passing on nothing.
//
//   test_security_gid_revoke <mount-a>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::Handoff_t;
using fsuser::tests::PlacementSize;

// A group nobody in this run carries, so a revoke naming it reaches nothing and says so.
constexpr std::uint32_t StrangerGid = 62200;

// A group this account holds besides the one it runs under, or 0 when it holds none. The child
// inherits the list, so what this answers about the parent holds for the row the child earns.
[[nodiscard]] std::uint32_t pickSupplementaryGid()
{
    const std::int32_t held = ::getgroups(0, nullptr);
    if (held <= 0)
    {
        return 0;
    }

    std::vector<::gid_t> groups(static_cast<std::size_t>(held));
    if (::getgroups(held, groups.data()) < 0)
    {
        return 0;
    }

    const auto running = ::getgid();
    for (const auto candidate : groups)
    {
        if (candidate != running && candidate != 0)
        {
            return static_cast<std::uint32_t>(candidate);
        }
    }
    return 0;
}

// The child maps once to earn a row of its own, then waits so that the row stands through the
// revokes the parent makes next.
[[noreturn]] void runConsumer(std::int32_t inherited, Handoff_t& handoff)
{
    handoff.ack.pass(fsuser::tests::probeReadMap(inherited, PlacementSize));
    static_cast<void>(handoff.begin.take());
    ::_exit(0);
}

void checkGidReachesASupplementaryHolder(fsuser::tests::Report& report,
                                         fsuser::tests::Mount& mount, std::uint32_t carried)
{
    report.section("a revoke naming a group its holder carries on the side");

    const auto name = caseName("security_gid_revoke");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);

        Handoff_t handoff;
        if (!handoff.isOpen())
        {
            report.check("the parent and child can talk", false);
            mount.unlink(name);
            return;
        }

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
        report.checkErrno("the helper writes the child a row of its own", firstMap == 0, firstMap);

        if (firstMap == 0)
        {
            // The child's row is the only one this region carries, so this says whether a group the
            // holder does not carry can reach a row all the same.
            report.checkRefusedWith("a group the holder does not carry reaches nothing",
                                    std::errc::no_such_file_or_directory,
                                    [&]
                                    {
                                        file.revokePermission(fsuser::AnyId, StrangerGid);
                                    });

            // The row was written under the child's own gid, so a success here comes from the group
            // list of the process behind it rather than from anything the row says.
            report.checkAccepted("the group the holder carries on the side reaches the row",
                                 [&]
                                 {
                                     file.revokePermission(fsuser::AnyId, carried);
                                 });

            report.checkRefusedWith("that row is gone, and it was there until then",
                                    std::errc::no_such_file_or_directory,
                                    [&]
                                    {
                                        file.revokePermission(fsuser::AnyId, carried);
                                    });
        }

        handoff.begin.pass(1);
        std::int32_t status = 0;
        const bool reaped = ::waitpid(child, &status, 0) == child;
        report.check("the child exits rather than being signalled",
                     reaped && (WIFEXITED(status) != 0));
    }
    catch (const std::exception& failure)
    {
        report.raised("security gid revoke", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return fsuser::tests::UsageStatus;
    }

    const std::uint32_t carried = pickSupplementaryGid();
    if (carried == 0)
    {
        std::printf(
            "test_security_gid_revoke: skipped, this account carries no group besides the "
            "one it runs under\n");
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkGidReachesASupplementaryHolder(report, mount, carried);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_gid_revoke", failure);
    }

    return report.summarise("test_security_gid_revoke");
}
