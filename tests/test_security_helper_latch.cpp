// SPDX-License-Identifier: Apache-2.0
//
// test_security_helper_latch -- whether one slow answer ends the mount for good.
//
// A request that times out marks the channel dead and every later request is refused without being
// sent, for one wait. Metadata writes go through the same channel, so the mark takes create, unlink,
// ftruncate and the permission verbs with it. The claim is that a helper which answers again is
// served again: after the wait one request goes through as a probe, and its answer clears the mark.
//
// Driving this stops the daemon for longer than the kernel waits, which changes the host. The case
// therefore does nothing unless it is told which process to stop, and reports a skip otherwise.
//
//   SECURITY_HELPER_PID=<pid> test_security_helper_latch <mount-a>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include "fs/errors.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"
#include "name.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::SkipStatus;

// Longer than the kernel's own wait, so the request under way gives up while the helper is stopped.
constexpr std::int32_t StoppedSeconds = 7;

// Together two kernel waits, which is the most a helper that is back should need to serve again.
constexpr std::int32_t RecoveryAttempts = 20;
constexpr std::int32_t RecoveryStepMs = 500;

// An order of magnitude under the kernel's own wait, so a refusal that took one is separated from
// the latch answering at once, with room for a loaded host.
constexpr std::int64_t PromptRefusalMs = 500;

// Names the process to stop. Absent means this host has not agreed to be wedged.
constexpr const char* HelperPidVariable = "SECURITY_HELPER_PID";

[[nodiscard]] std::string makeDaemonStatePath(const fsuser::tests::Mount& mount)
{
    return "/sys/fs/" FS_NAME_STR "/node" + std::to_string(mount.getNodeId()) + "/daemon_state";
}

// True when the channel has latched its helper as dead. Answers false when the file cannot be read,
// which a caller separates with its own check.
[[nodiscard]] bool readHelperDead(const fsuser::tests::Mount& mount)
{
    const auto text = fsuser::tests::readWholeLine(makeDaemonStatePath(mount).c_str());
    return text.find("helper_dead=1") != std::string::npos;
}

void checkChannelRecoversFromOneTimeout(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                        const char* pidText)
{
    report.section("a channel that answered late once");

    const auto helperPid = static_cast<::pid_t>(std::atoi(pidText));
    report.check("the helper pid parses", helperPid > 1);
    if (helperPid <= 1)
    {
        return;
    }
    const bool alreadyDead = readHelperDead(mount);
    report.check("the channel starts with a live helper", !alreadyDead);
    if (alreadyDead)
    {
        return;
    }

    const auto name = caseName("security_helper_latch");
    try
    {
        // Unprivileged, this is EPERM against the daemon's account, and every check below would
        // then pass without the latch ever being driven.
        const bool stopped = ::kill(helperPid, SIGSTOP) == 0;
        report.check("the helper stops", stopped);
        if (!stopped)
        {
            return;
        }
        std::int32_t createErrno = 0;
        try
        {
            auto file = mount.open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        }
        catch (const fsuser::FsCodedError& refused)
        {
            createErrno = refused.code().value();
        }
        // What the latch is for: inside the wait that follows a timeout the refusal arrives without
        // the request being sent, so it costs nothing like a wait of its own.
        const auto beganAt = std::chrono::steady_clock::now();
        std::int32_t latchedErrno = 0;
        try
        {
            auto second = mount.open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        }
        catch (const fsuser::FsCodedError& refused)
        {
            latchedErrno = refused.code().value();
        }
        const auto refusedAfterMs = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beganAt)
                .count());

        std::this_thread::sleep_for(std::chrono::seconds(StoppedSeconds));
        report.check("the helper resumes", ::kill(helperPid, SIGCONT) == 0);

        report.checkErrno("the create under a stopped helper timed out", createErrno == ETIMEDOUT,
                          createErrno);
        report.checkErrno("a create inside the deny window is refused as well",
                          latchedErrno == ETIMEDOUT, latchedErrno);
        report.check("that refusal costs no wait of its own: " + std::to_string(refusedAfterMs) + " ms",
                     refusedAfterMs < PromptRefusalMs);

        // The channel answers at once for one kernel wait after the timeout, then lets a request
        // through to find out whether the helper is back. So the create is retried across that wait.
        std::int32_t afterErrno = ETIMEDOUT;
        for (std::int32_t attempt = 0; attempt < RecoveryAttempts && afterErrno == ETIMEDOUT; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(RecoveryStepMs));
            afterErrno = 0;
            try
            {
                auto file = mount.open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
            }
            catch (const fsuser::FsCodedError& refused)
            {
                afterErrno = refused.code().value();
            }
        }
        report.checkErrno("a create works again within one kernel wait of the helper's return",
                          afterErrno == 0, afterErrno);
        report.check("the channel forgets the timeout once a request is answered again",
                     !readHelperDead(mount));
    }
    catch (const std::exception& failure)
    {
        report.raised("security helper latch", failure);
    }

    try
    {
        mount.unlink(name);
    }
    catch (const std::exception&)  // NOLINT(bugprone-empty-catch) the name may never have landed
    {
    }
}

// sudo runs this as root so the daemon takes the SIGSTOP, but the rules name the invoking account
// and the kernel attests the real uid. So the real ids drop to the invoker while the effective stay.
bool dropRealIdsToInvoker()
{
    const char* uidText = std::getenv("SUDO_UID");
    const char* gidText = std::getenv("SUDO_GID");
    if (::geteuid() != 0 || uidText == nullptr || gidText == nullptr)
    {
        return true;
    }
    const auto realUid = static_cast<::uid_t>(std::atoi(uidText));
    const auto realGid = static_cast<::gid_t>(std::atoi(gidText));
    return ::setregid(realGid, 0) == 0 && ::setreuid(realUid, 0) == 0;
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    const char* pidText = std::getenv(HelperPidVariable);
    if (pidText == nullptr)
    {
        std::printf(
            "test_security_helper_latch: %s is unset, so this host was not asked to be "
            "wedged\n",
            HelperPidVariable);
        return SkipStatus;
    }
    if (!dropRealIdsToInvoker())
    {
        std::printf("test_security_helper_latch: could not drop the real ids to the invoker\n");
        return 2;
    }

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkChannelRecoversFromOneTimeout(report, mount, pidText);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_helper_latch", failure);
    }

    return report.summarise("test_security_helper_latch");
}
