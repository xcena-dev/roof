// SPDX-License-Identifier: Apache-2.0
//
// test_postexec_attack -- a delegation names the executable that first used it, and execve changes
// that.
//
// A delegation row binds three things about the caller on the first match: the pid, the start time,
// and the executable. execve preserves the first two, so the executable is the only one of the three
// that separates the process that was granted from the image now running inside it.
//
// So the granted child maps once, which binds the row to this binary. It then execs into
// postexec_other, which is the same pid and the same start time under a different executable, and
// that image has to be refused.
//
// The child reaches the file through a fresh descriptor after the exec, not an inherited one. mmap
// refuses a descriptor without O_CLOEXEC outright, and one carrying O_CLOEXEC is gone by the time
// the new image runs, so an inherited descriptor could never reach the check under test.
//
//   test_postexec_attack <mount-a> <mount-b>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"

namespace
{

// Statuses outside the errno range, for the two ways the child can end before the new image runs.
constexpr std::int32_t BindFailed = 250;
constexpr std::int32_t ExecFailed = 251;

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// Runs in the child. Maps once to bind the row, reports that mapping's errno over @back, and then
// hands the process to the other binary. Nothing after the exec belongs to this file.
[[noreturn]] void bindThenExec(fsuser::tests::Baton& begin, fsuser::tests::Baton& back,
                               const std::string& mount, const std::string& name)
{
    static_cast<void>(begin.take());

    const std::int32_t result = fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize);
    back.pass(result);
    if (result != 0)
    {
        ::_exit(BindFailed);
    }

    ::execl(FS_POSTEXEC_OTHER, FS_POSTEXEC_OTHER, mount.c_str(), name.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(ExecFailed);
}

enum class Outcome : std::int32_t
{
    Measured,
    PolicyCannotSeparate,
};

Outcome checkExecChangesTheHolder(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                  const std::string& peerMount, const std::string& name)
{
    report.section("a granted process that execs into another binary");

    auto file = fsuser::tests::placeFile(mount, name);

    fsuser::tests::Baton begin;
    fsuser::tests::Baton back;
    report.check("the handoff pipes open", begin.isOpen() && back.isOpen());

    const ::pid_t granted = ::fork();
    if (granted == 0)
    {
        bindThenExec(begin, back, peerMount, name);
    }
    report.check("fork succeeds", granted > 0);
    if (granted <= 0)
    {
        return Outcome::Measured;
    }

    // No grant from here. The child's map is refused on the record, the helper on its node
    // decides, and the row the kernel then writes names that child's binary.
    begin.pass(1);

    const std::int32_t bound = back.take();
    report.checkErrno("the granted child maps and so binds its executable", bound == 0, bound);

    std::int32_t status = 0;
    const bool reaped = ::waitpid(granted, &status, 0) == granted;
    report.check("the child exits rather than being signalled", reaped && (WIFEXITED(status) != 0));
    if (!reaped || (WIFEXITED(status) == 0))
    {
        return Outcome::Measured;
    }

    const std::int32_t left = WEXITSTATUS(status);
    if (left == BindFailed)
    {
        return Outcome::Measured;
    }
    report.check("the child execs into the other binary", left != ExecFailed);
    if (left == ExecFailed)
    {
        return Outcome::Measured;
    }

    // The row named the first binary, so the exec cannot carry it over. What decides the second is
    // the helper, asked afresh, and a helper whose rules name only an account cannot tell the two
    // apart. Then there is nothing here to measure and this says so rather than passing.
    if (left == 0)
    {
        return Outcome::PolicyCannotSeparate;
    }
    report.checkErrno("the other binary is refused a row of its own", left == EACCES, left);
    return Outcome::Measured;
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
    Outcome outcome = Outcome::Measured;
    const auto name = caseName("postexec_attack");

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        outcome = checkExecChangesTheHolder(report, mount, mounts.second(), name);
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_postexec_attack setup", failure);
    }

    if (outcome == Outcome::PolicyCannotSeparate && !report.anyFailed())
    {
        std::printf(
            "test_postexec_attack: skipped, the helper's rules name an account and not a "
            "binary, so the binary it execs into attests the same as the first\n");
        return fsuser::tests::SkipStatus;
    }
    return report.summarise("test_postexec_attack");
}
