// SPDX-License-Identifier: Apache-2.0
//
// test_exec_generation -- a re-exec of the same binary does not keep serving from the delegation
// row the earlier image earned, because the row also binds the execve generation.
//
// pid, start time and exe inode survive a self re-exec, so the helper's own asked count is what
// shows the row stopped answering: a fresh ask is the only way that count moves.
//
//   test_exec_generation <mount-a> <mount-b>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"

namespace
{

// Statuses outside the errno range, for the two ways the child can end before the second image runs.
constexpr std::int32_t BindFailed = 250;
constexpr std::int32_t ExecFailed = 251;

// Well clear of any account another case in this suite grants, so the fill below never rewrites a
// row that was not its own.
constexpr std::uint32_t FillFirstUid = 62000;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

constexpr const char* SecondGenerationToken = "post-exec";

// Runs after the re-exec. Binds a second time under the new generation and exits with the errno
// the kernel gave, or 0 when the mapping landed.
[[noreturn]] void bindAsSecondGeneration(const std::string& mount, const std::string& name)
{
    ::_exit(fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize));
}

// Runs in the child. Binds once under this generation and passes the errno over @back, binds again
// with no exec between and passes whether that one asked the helper over the same @back, then
// re-execs into its own binary so the same exe inode runs again under a new one.
[[noreturn]] void bindThenReexec(fsuser::tests::Baton& begin, fsuser::tests::Baton& back,
                                 std::uint32_t peerNode, const std::string& mount,
                                 const std::string& name)
{
    static_cast<void>(begin.take());

    const std::int32_t result = fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize);
    back.pass(result);
    if (result != 0)
    {
        ::_exit(BindFailed);
    }

    // Same pid, birth time, exe inode and generation as the bind just above, so the row alone
    // should answer this one and the helper should never be asked again.
    const auto askedBeforeControl = fsuser::tests::readDaemonAsked(peerNode);
    static_cast<void>(fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize));
    const auto askedAfterControl = fsuser::tests::readDaemonAsked(peerNode);

    std::int32_t rose = -1;
    if (askedBeforeControl && askedAfterControl)
    {
        rose = (*askedAfterControl > *askedBeforeControl) ? 1 : 0;
    }
    back.pass(rose);

    ::execl("/proc/self/exe", "/proc/self/exe", SecondGenerationToken, mount.c_str(), name.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(ExecFailed);
}

// Grants @count more accounts a row on @file, each a distinct uid so a rerun never rewrites one it
// already wrote. Answers how many landed before the first refusal.
[[nodiscard]] std::uint32_t fillAccountRows(fsuser::File& file, std::uint32_t count)
{
    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        try
        {
            file.grantPermission(FillFirstUid + index, fsuser::AnyId, fsuser::Permission::Read);
            ++written;
        }
        catch (const std::exception&)
        {
            break;
        }
    }
    return written;
}

void checkReexecRewritesTheSameSlot(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                    const std::string& peerMount, const std::string& name)
{
    report.section("a process that re-execs its own binary");

    auto file = fsuser::tests::placeFile(mount, name);

    // Read once before the fork, since node id is a property of the mount table and not of the
    // mount point's name, and the child needs the same value to read its own channel counter.
    const auto peerNode = fsuser::tests::Mount(peerMount).getNodeId();

    fsuser::tests::Baton begin;
    fsuser::tests::Baton back;
    report.check("the handoff pipes open", begin.isOpen() && back.isOpen());

    const ::pid_t granted = ::fork();
    if (granted == 0)
    {
        bindThenReexec(begin, back, peerNode, peerMount, name);
    }
    report.check("fork succeeds", granted > 0);
    if (granted <= 0)
    {
        return;
    }

    // No grant from here. The child's map is refused on the record, the helper on its node
    // decides, and the row the kernel then writes names that child's first generation.
    begin.pass(1);

    const std::int32_t bound = back.take();
    report.checkErrno("the first generation maps and so earns a row of its own", bound == 0, bound);

    // @back carries a second value right behind the first: whether the no-exec bind that follows
    // asked the helper again.
    const std::int32_t secondBindRose = back.take();
    if (bound == 0)
    {
        report.check("a second bind without an exec is answered by the row", secondBindRose == 0);
    }

    // Read once the first generation is bound and before the exec, so what follows counts only
    // what the second image caused.
    const auto askedBeforeExec = fsuser::tests::readDaemonAsked(peerNode);
    report.check("the node's channel reports what it has been asked", askedBeforeExec.has_value());

    std::int32_t status = 0;
    const bool reaped = ::waitpid(granted, &status, 0) == granted;
    report.check("the child exits rather than being signalled", reaped && (WIFEXITED(status) != 0));
    if (!reaped || (WIFEXITED(status) == 0))
    {
        return;
    }

    const std::int32_t left = WEXITSTATUS(status);
    if (left == BindFailed)
    {
        return;
    }
    report.check("the child re-execs its own binary", left != ExecFailed);
    if (left == ExecFailed)
    {
        return;
    }

    // Whichever way the second generation is answered, reaching an answer at all is the claim: a
    // row that still matched it would have skipped the helper and left this count where it was.
    const auto askedAfterExec = fsuser::tests::readDaemonAsked(peerNode);
    report.check("the second generation asks the helper again rather than reusing the row",
                 askedBeforeExec && askedAfterExec && *askedAfterExec > *askedBeforeExec);
    report.note("the second generation's own bind answered errno=" + std::to_string(left));

    // Neither generation took a second slot of its own on this file, so the table's other slots
    // still hold exactly one fewer than the full count.
    const std::uint32_t remaining = fsuser::MaxDelegations - 1;
    const std::uint32_t written = fillAccountRows(file, remaining);
    report.check("a re-exec does not spend a second slot of its own: " + std::to_string(written) +
                     " of " + std::to_string(remaining) + " still fit",
                 written == remaining);

    report.checkRefusedWith("the grant past the last slot answers ENOSPC",
                            std::errc::no_space_on_device,
                            [&]
                            {
                                file.grantPermission(FillFirstUid + remaining, fsuser::AnyId,
                                                     fsuser::Permission::Read);
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == SecondGenerationToken)
    {
        if (argc < 4)
        {
            std::fprintf(stderr, "usage: %s %s <mount> <name>\n", argv[0], SecondGenerationToken);
            return 2;
        }
        bindAsSecondGeneration(argv[2], argv[3]);
    }

    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 2, mounts))
    {
        return 2;
    }

    fsuser::tests::Report report;
    const auto name = caseName("exec_generation");

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkReexecRewritesTheSameSlot(report, mount, mounts.second(), name);
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_exec_generation setup", failure);
    }

    return report.summarise("test_exec_generation");
}
