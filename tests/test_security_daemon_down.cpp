// SPDX-License-Identifier: Apache-2.0
//
// test_security_daemon_down -- what a mount answers while its daemon is gone.
//
// Two claims, and the second is the one a blanket refusal would fail. A mount with no daemon refuses
// rather than opens: a create, a metadata write and an access no row covers all answer EAGAIN. An
// access a row already covers still goes through, because that row is a decision the daemon has
// already made and nothing has to be asked again.
//
// systemctl stop and not SIGSTOP. Stopping the process releases the channel, and a released channel
// is what answers EAGAIN. A process left stopped still holds the channel open, so the request is
// queued and gives up with ETIMEDOUT instead, which is a different claim and a different case.
//
// The case takes the node's real daemon away for its run, so it refuses to drive anything unless
// told which node it may take. It shares that variable with the case that stands in for a helper,
// since the consent is the same one.
//
//   SECURITY_FAKE_HELPER_NODE=<node> test_security_daemon_down <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"
#include "name.h"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::daemonStatePath;
using fsuser::tests::placeFile;
using fsuser::tests::PlacementSize;
using fsuser::tests::readTagged;
using fsuser::tests::readWholeLine;
using fsuser::tests::Report;
using fsuser::tests::SkipStatus;

// Names the node whose daemon this run may take away. Absent means this host has not agreed.
constexpr const char* HelperNodeVariable = "SECURITY_FAKE_HELPER_NODE";

// The account the row names, and one that holds no row. Well clear of any the host itself hands
// out, so a row written under either names nobody who could then use it.
constexpr ::uid_t RowUid = 60200;
constexpr ::uid_t StrangerUid = 60201;

// How long the case waits for the channel to change hands, which is a unit stopping or starting.
constexpr std::int32_t ChannelDeadlineMs = 8000;
constexpr std::int32_t ChannelPollMs = 50;

// Stops @unit for its lifetime and starts it again on the way out, whichever way that happens.
class HelperServiceGuard
{
public:
    explicit HelperServiceGuard(std::string unit)
        : unit_{std::move(unit)},
          stopped_{runSystemctl("stop", unit_) == 0}
    {
    }

    ~HelperServiceGuard()
    {
        if (stopped_ && runSystemctl("start", unit_) != 0)
        {
            std::fprintf(stderr, "test_security_daemon_down: could not restart %s\n", unit_.c_str());
        }
    }

    HelperServiceGuard(const HelperServiceGuard&) = delete;
    HelperServiceGuard& operator=(const HelperServiceGuard&) = delete;
    HelperServiceGuard(HelperServiceGuard&&) = delete;
    HelperServiceGuard& operator=(HelperServiceGuard&&) = delete;

    [[nodiscard]] bool stopped() const noexcept
    {
        return stopped_;
    }

private:
    [[nodiscard]] static std::int32_t runSystemctl(const char* verb, const std::string& unit)
    {
        const ::pid_t child = ::fork();
        if (child < 0)
        {
            return -1;
        }
        if (child == 0)
        {
            ::execlp("systemctl", "systemctl", verb, unit.c_str(), nullptr);
            ::_exit(127);
        }
        std::int32_t status = 0;
        if (::waitpid(child, &status, 0) != child || !WIFEXITED(status))
        {
            return -1;
        }
        return WEXITSTATUS(status);
    }

private:
    std::string unit_;
    bool stopped_;
};

// Whether the kernel still has a reader on @node's channel. That bit and not the unit's own state is
// what every refusal below depends on.
[[nodiscard]] bool channelHasReader(std::uint32_t node)
{
    const auto found = readTagged(readWholeLine(daemonStatePath(node).c_str()), "reader_open=");
    return found.value_or(0) == 1;
}

// Bounded, so a unit that never changes hands ends the case with a failed check rather than hanging
// it. Answers whether the channel reached @wanted before the deadline.
[[nodiscard]] bool waitForChannelReader(std::uint32_t node, bool wanted)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ChannelDeadlineMs);
    while (std::chrono::steady_clock::now() < until)
    {
        if (channelHasReader(node) == wanted)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ChannelPollMs));
    }
    return false;
}

// Reads one byte, which is the other data path that asks the daemon when no row covers the caller.
// Answers 0 or the errno pread left behind.
[[nodiscard]] std::int32_t probeReadByte(std::int32_t descriptor)
{
    char byte = 0;
    errno = 0;
    return ::pread(descriptor, &byte, 1, 0) < 0 ? errno : 0;
}

// Runs @probe in a child that has become @account and answers what the probe returned. The
// descriptor stays the parent's, opened before the account changed, so what the kernel judges is
// the account alone.
template <typename T_Probe>
[[nodiscard]] std::int32_t answerAsAccount(::uid_t account, T_Probe&& probe)
{
    fsuser::tests::Baton outcome;
    if (!outcome.isOpen())
    {
        return -1;
    }

    const ::pid_t child = ::fork();
    if (child < 0)
    {
        return -1;
    }
    if (child == 0)
    {
        if (::setregid(account, account) != 0 || ::setreuid(account, account) != 0)
        {
            outcome.pass(EPERM);
            ::_exit(3);
        }
        outcome.pass(probe());
        ::_exit(0);
    }

    const std::int32_t answered = outcome.take();
    static_cast<void>(::waitpid(child, nullptr, 0));
    return answered;
}

// ── Claim 1: a row the daemon already wrote still answers ────────────────

void checkACoveredAccessStillGoesThrough(Report& report, fsuser::File& file)
{
    report.section("an access a row already covers, with no daemon behind it");

    const auto mapped =
        answerAsAccount(RowUid, [&]
                        {
                            return fsuser::tests::probeReadMap(file.get(), PlacementSize);
                        });
    report.checkErrno("a mapping the row covers goes through with no daemon to ask", mapped == 0,
                      mapped);

    const auto readOne = answerAsAccount(RowUid, [&]
                                         {
                                             return probeReadByte(file.get());
                                         });
    report.checkErrno("and so does a read on the same row", readOne == 0, readOne);
}

// ── Claim 2: everything that needs an answer is refused ──────────────────

void checkNothingElseOpens(Report& report, fsuser::tests::Mount& mount, fsuser::File& file)
{
    report.section("what needs the daemon while it is gone");

    const auto strangerMapped =
        answerAsAccount(StrangerUid, [&]
                        {
                            return fsuser::tests::probeReadMap(file.get(), PlacementSize);
                        });
    report.checkErrno("an access no row covers is refused rather than opened",
                      strangerMapped == EAGAIN, strangerMapped);

    const auto second = caseName("daemon_down_create");
    report.checkRefusedWith("a create is refused", std::errc::resource_unavailable_try_again,
                            [&]
                            {
                                auto made = mount.open(second, O_CREAT | O_RDWR, 0644);
                            });

    report.checkRefusedWith("a metadata write is refused",
                            std::errc::resource_unavailable_try_again,
                            [&]
                            {
                                file.grantPermission(StrangerUid, fsuser::AnyId,
                                                     fsuser::Permission::Read);
                            });
}

void checkAMountWithNoDaemonRefuses(Report& report, fsuser::tests::Mount& mount, std::uint32_t node)
{
    // Placed and granted while the daemon still serves: both are metadata writes, and the row is
    // what the first claim needs already standing when the daemon goes.
    const auto name = caseName("daemon_down");
    auto file = placeFile(mount, name);
    {
        auto mapped = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the region maps for the first write", mapped.isMapped());
    }
    file.grantPermission(RowUid, fsuser::AnyId, fsuser::Permission::Read);

    const std::string unit = std::string{DAEMON_NAME_STR} + "@" + std::to_string(node) + ".service";
    {
        // Read while the daemon serves, so a channel state that cannot be read at all shows up here
        // and not as a stop that seemed to take.
        report.check("the daemon is on the channel before the stop", channelHasReader(node));
        const HelperServiceGuard guard{unit};
        report.check("the daemon stops", guard.stopped());
        if (guard.stopped())
        {
            report.check("the kernel is left with no reader on the channel",
                         waitForChannelReader(node, false));
            checkACoveredAccessStillGoesThrough(report, file);
            checkNothingElseOpens(report, mount, file);
        }
    }

    report.check("the daemon is back on the channel", waitForChannelReader(node, true));

    // The unlink below is a metadata write, so it needs the daemon the guard above just restarted.
    try
    {
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("removing the region this case placed", failure);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    const char* nodeText = std::getenv(HelperNodeVariable);
    if (nodeText == nullptr)
    {
        std::printf("test_security_daemon_down: %s is unset, so this host was not asked to be driven\n",
                    HelperNodeVariable);
        return SkipStatus;
    }
    if (::geteuid() != 0)
    {
        std::printf("test_security_daemon_down: needs root to stop the daemon and borrow an account\n");
        return SkipStatus;
    }
    const auto node = static_cast<std::uint32_t>(std::atoi(nodeText));
    if (node == 0)
    {
        std::printf("test_security_daemon_down: %s does not name a node\n", HelperNodeVariable);
        return 2;
    }
    if (!fsuser::tests::dropRealIdsToInvoker())
    {
        std::printf(
            "test_security_daemon_down: SUDO_UID and SUDO_GID name the account the region is "
            "placed under, and this run has neither\n");
        return SkipStatus;
    }

    Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkAMountWithNoDaemonRefuses(report, mount, node);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_daemon_down", failure);
    }

    return report.summarise("test_security_daemon_down");
}
