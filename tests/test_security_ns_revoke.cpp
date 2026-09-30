// SPDX-License-Identifier: Apache-2.0
//
// test_security_ns_revoke -- whether a revoke issued from another pid namespace reaches the process
// row it names.
//
// A row records its holder's tgid as the initial namespace numbers it. A revoke naming a group the
// row does not carry has to ask the holder process itself, and that question is a lookup of the
// stored number. Asked in a container's own numbering, the number names another process or none.
//
// The case gives the holder and the revoker one group on the side, then drives the revoke from a
// process one pid namespace deeper. Both steps need privilege, so a run without it reports a skip.
//
//   sudo test_security_ns_revoke <mount-a>

#include <grp.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "fs/errors.hpp"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::Handoff_t;
using fsuser::tests::PlacementSize;

// The group this run hands the holder and the revoker. Nothing runs under it, so the row records it
// nowhere and the live process is the only place the revoke can read it off.
constexpr std::uint32_t CarriedGid = 62201;

// What the nested process reports when it never reached the revoke at all.
constexpr std::int32_t NoAnswer = -1;

// Adds @gid to what this process already carries, which is what the kernel reads off a live holder.
[[nodiscard]] bool carryGroup(std::uint32_t gid)
{
    const std::int32_t held = ::getgroups(0, nullptr);
    if (held < 0)
    {
        return false;
    }

    std::vector<::gid_t> groups(static_cast<std::size_t>(held) + 1);
    if (held > 0 && ::getgroups(held, groups.data()) < 0)
    {
        return false;
    }
    groups.back() = static_cast<::gid_t>(gid);
    return ::setgroups(groups.size(), groups.data()) == 0;
}

// The child maps once to earn a row of its own, then waits so that the row stands through the
// revokes the parent drives next.
[[noreturn]] void runConsumer(std::int32_t inherited, Handoff_t& handoff)
{
    if (!carryGroup(CarriedGid))
    {
        handoff.ack.pass(EPERM);
        ::_exit(3);
    }

    handoff.ack.pass(fsuser::tests::probeReadMap(inherited, PlacementSize));
    static_cast<void>(handoff.begin.take());
    ::_exit(0);
}

// The errno the revoke left, or 0 when it went through.
[[nodiscard]] std::int32_t revokeAndReport(fsuser::File& file, std::uint32_t uid, std::uint32_t gid)
{
    try
    {
        file.revokePermission(uid, gid);
    }
    catch (const fsuser::FsCodedError& refusal)
    {
        return refusal.code().value();
    }
    catch (const std::exception&)
    {
        return NoAnswer;
    }
    return 0;
}

// The same revoke from a process whose pid namespace is not the one that numbered the row. unshare
// moves the caller's children rather than the caller, so the revoke runs one fork deeper.
[[nodiscard]] std::int32_t revokeFromNestedNamespace(fsuser::File& file, std::uint32_t uid,
                                                     std::uint32_t gid)
{
    fsuser::tests::Baton answer;
    if (!answer.isOpen())
    {
        return NoAnswer;
    }

    const ::pid_t outer = ::fork();
    if (outer < 0)
    {
        return NoAnswer;
    }
    if (outer == 0)
    {
        // The group is what lets this account name the revoke without ADMIN, and the holder's own
        // list is what the row is reached through.
        if (!carryGroup(gid) || ::unshare(CLONE_NEWPID) != 0)
        {
            answer.pass(NoAnswer);
            ::_exit(3);
        }

        const ::pid_t inner = ::fork();
        if (inner < 0)
        {
            answer.pass(NoAnswer);
            ::_exit(3);
        }
        if (inner == 0)
        {
            answer.pass(revokeAndReport(file, uid, gid));
            ::_exit(0);
        }

        static_cast<void>(::waitpid(inner, nullptr, 0));
        ::_exit(0);
    }

    const std::int32_t answered = answer.take();
    static_cast<void>(::waitpid(outer, nullptr, 0));
    return answered;
}

void checkANestedRevokeReachesTheHolder(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                        std::uint32_t account)
{
    report.section("a revoke driven from a nested pid namespace");

    const auto name = caseName("security_ns_revoke");

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
            // The holder's row is the only one this region carries, and the group naming it is
            // carried by the holder alone. So the number the lookup asks with is what decides.
            const std::int32_t reached = revokeFromNestedNamespace(file, account, CarriedGid);
            report.check("the nested process reaches the revoke", reached != NoAnswer);
            report.checkErrno("a revoke from another pid namespace reaches the holder's row",
                              reached == 0, reached);

            const std::int32_t again = revokeFromNestedNamespace(file, account, CarriedGid);
            report.checkErrno("that row is gone, and it was there until then", again == ENOENT,
                              again);
        }

        handoff.begin.pass(1);
        std::int32_t status = 0;
        const bool reaped = ::waitpid(child, &status, 0) == child;
        report.check("the child exits rather than being signalled",
                     reaped && (WIFEXITED(status) != 0));
    }
    catch (const std::exception& failure)
    {
        report.raised("security ns revoke", failure);
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

    if (::geteuid() != 0)
    {
        std::printf(
            "test_security_ns_revoke: needs root to enter a pid namespace and to hand a process "
            "a group\n");
        return fsuser::tests::SkipStatus;
    }
    if (!fsuser::tests::dropRealIdsToInvoker())
    {
        std::printf(
            "test_security_ns_revoke: SUDO_UID and SUDO_GID name the account the region is "
            "placed under, and this run has neither\n");
        return fsuser::tests::SkipStatus;
    }

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkANestedRevokeReachesTheHolder(report, mount,
                                           static_cast<std::uint32_t>(::getuid()));
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_ns_revoke", failure);
    }

    return report.summarise("test_security_ns_revoke");
}
