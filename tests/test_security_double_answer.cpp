// SPDX-License-Identifier: Apache-2.0
//
// test_security_double_answer -- whether a second answer to an already-answered request can land.
//
// A helper that answers twice must not get two verdicts through: the first one stands and the
// second reaches nobody. The kernel reports a stale answer as success, so the outcome is read from
// which verdict the caller saw, the queue depth afterwards, and the log.
//
// The case stands in for the real helper, so it stops one for the run and refuses to drive
// anything unless told which node's it may take over.
//
// The stand-in grants every turn it is asked for without taking one, so this case is sound only
// while its node is the mount's only writer. It runs serially for that reason.
//
//   SECURITY_FAKE_HELPER_NODE=<node> test_security_double_answer <mount-a>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// daemon_uapi.h speaks C11's _Static_assert, which this tree's strict C++17 build (CXX_EXTENSIONS
// OFF) has no keyword for.
#ifndef _Static_assert
#define _Static_assert static_assert
#endif
#include "daemon_uapi.h"
#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/sysfs.hpp"
#include "name.h"
#include "uapi.h"

namespace
{

using fsuser::tests::Baton;
using fsuser::tests::caseName;
using fsuser::tests::daemonStatePath;
using fsuser::tests::PlacementSize;
using fsuser::tests::readTagged;
using fsuser::tests::readWholeLine;
using fsuser::tests::Report;

// Names the node whose real helper this run may stop. Absent means this host has not agreed.
constexpr const char* FakeHelperNodeVariable = "SECURITY_FAKE_HELPER_NODE";

// A losing streak proves the claim; a single pass does not, since the reversal it looks for is rare
// even against the queue this case exists to rule out.
constexpr std::int32_t ReversalAttempts = 100;

// Room for the largest request shape the kernel sends, which a read needs before the header says
// which shape arrived.
constexpr std::size_t RequestFrameBytes = sizeof(fs_daemon_hdr) + sizeof(fs_daemon_access_request);

// How long a caller waits for the ACCESS request its own forked child should have raised.
constexpr std::chrono::milliseconds AccessRequestWait{5000};

struct AccessRequestFrame_t
{
    fs_daemon_hdr hdr{};
    fs_daemon_access_request payload{};
};

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
            std::fprintf(stderr, "test_security_double_answer: could not restart %s\n",
                         unit_.c_str());
        }
    }

    HelperServiceGuard(const HelperServiceGuard&) = delete;
    HelperServiceGuard& operator=(const HelperServiceGuard&) = delete;

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

// O_NONBLOCK, because the frame a poll reported can be gone before the read runs: a read that waits
// for the next one instead has no deadline and no stop.
[[nodiscard]] std::int32_t openHelperDevice(std::uint32_t node)
{
    const std::string path = "/dev/" DAEMON_NAME_STR "-" + std::to_string(node);
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
}

// How long a read that must not wait is allowed to take before this case stops waiting on it.
constexpr std::uint32_t EmptyReadGuardSec = 2;

// The errno a read of the empty channel left, or 0 when a frame arrived after all. The timer is
// what turns a channel opened the wrong way into a failure rather than a case that never ends.
[[nodiscard]] std::int32_t readTheEmptyChannel(std::int32_t device)
{
    struct ::sigaction waking = {};
    waking.sa_handler = [](std::int32_t)
    {
    };
    struct ::sigaction before = {};
    if (::sigaction(SIGALRM, &waking, &before) != 0)
    {
        return -1;
    }

    std::array<std::uint8_t, RequestFrameBytes> unused{};
    static_cast<void>(::alarm(EmptyReadGuardSec));
    errno = 0;
    const auto got = ::read(device, unused.data(), unused.size());
    const std::int32_t left = errno;
    static_cast<void>(::alarm(0));
    static_cast<void>(::sigaction(SIGALRM, &before, nullptr));

    return got < 0 ? left : 0;
}

// The one handshake a channel needs before it queues anything to this case's stand-in helper. LOCK
// rides beside ACCESS because the kernel refuses at once what the channel never declared.
[[nodiscard]] bool sendHello(std::int32_t device)
{
    fs_daemon_hdr hdr{};
    hdr.magic = FS_DAEMON_MAGIC;
    hdr.version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hdr.type = static_cast<__u32>(FS_DAEMON_MSG_HELLO);
    hdr.payload_len = static_cast<__u32>(sizeof(fs_daemon_hello));

    fs_daemon_hello hello{};
    hello.protocol_version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hello.capabilities =
        static_cast<__u64>(FS_DAEMON_CAP_ACCESS_REQUEST) | static_cast<__u64>(FS_DAEMON_CAP_LOCK);
    hello.helper_pid = static_cast<__u32>(::getpid());

    std::array<std::uint8_t, sizeof(hdr) + sizeof(hello)> frame{};
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &hello, sizeof(hello));

    return ::write(device, frame.data(), frame.size()) == static_cast<::ssize_t>(frame.size());
}

// The turn, granted without one ever being taken. Sound only while this node is the mount's only
// writer, which is the condition this case runs under.
[[nodiscard]] bool writeLockResponse(std::int32_t device, __u64 seq)
{
    fs_daemon_hdr hdr{};
    hdr.magic = FS_DAEMON_MAGIC;
    hdr.version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hdr.type = static_cast<__u32>(FS_DAEMON_MSG_LOCK_RESPONSE);
    hdr.payload_len = static_cast<__u32>(sizeof(fs_daemon_lock_response));
    hdr.seq = seq;

    fs_daemon_lock_response resp{};
    resp.status = 0;

    std::array<std::uint8_t, sizeof(hdr) + sizeof(resp)> frame{};
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &resp, sizeof(resp));

    return ::write(device, frame.data(), frame.size()) == static_cast<::ssize_t>(frame.size());
}

// Answers @seq with a grant of FS_PERM_READ when @allow, EACCES otherwise. False when the write did
// not carry the whole frame, never when the kernel found nothing left to attach it to.
[[nodiscard]] bool writeAccessResponse(std::int32_t device, __u64 seq, bool allow)
{
    fs_daemon_hdr hdr{};
    hdr.magic = FS_DAEMON_MAGIC;
    hdr.version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hdr.type = static_cast<__u32>(FS_DAEMON_MSG_ACCESS_RESPONSE);
    hdr.payload_len = static_cast<__u32>(sizeof(fs_daemon_response));
    hdr.seq = seq;

    fs_daemon_response resp{};
    resp.status = allow ? 0 : -EACCES;
    resp.granted_perms = allow ? static_cast<__u32>(FS_PERM_READ) : 0U;

    std::array<std::uint8_t, sizeof(hdr) + sizeof(resp)> frame{};
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &resp, sizeof(resp));

    return ::write(device, frame.data(), frame.size()) == static_cast<::ssize_t>(frame.size());
}

// How long the pump sits in one poll before it looks at whether it was asked to stop.
constexpr std::int32_t PumpPollMs = 100;

// The channel's only reader while this case holds it. The kernel asks for a turn to write the row
// an allow earns, and it asks while the caller that earned it is still parked on its own answer.
class ChannelPump
{
public:
    explicit ChannelPump(std::int32_t device)
        : device_{device},
          worker_{[this]
                  {
                      run();
                  }}
    {
    }

    ~ChannelPump()
    {
        stopping_.store(true);
        worker_.join();
    }

    ChannelPump(const ChannelPump&) = delete;
    ChannelPump& operator=(const ChannelPump&) = delete;

    // The next ACCESS request the kernel sent, or false when none arrived within @within.
    [[nodiscard]] bool takeAccessRequest(AccessRequestFrame_t& into,
                                         std::chrono::milliseconds within)
    {
        std::unique_lock<std::mutex> holding{guard_};
        if (!arrived_.wait_for(holding, within, [this]
                               {
                                   return !waiting_.empty();
                               }))
        {
            return false;
        }
        into = waiting_.front();
        waiting_.pop_front();
        return true;
    }

private:
    // Waits in poll rather than in read, since closing the channel never wakes a thread already
    // inside read.
    void run()
    {
        std::array<std::uint8_t, RequestFrameBytes> buffer{};
        while (!stopping_.load())
        {
            ::pollfd watched{device_, POLLIN, 0};
            const auto ready = ::poll(&watched, 1, PumpPollMs);
            if (ready < 0)
            {
                return;
            }
            if (ready == 0)
            {
                continue;
            }

            const auto got = ::read(device_, buffer.data(), buffer.size());
            if (got < 0 && (errno == EAGAIN || errno == EINTR))
            {
                // The requester took its frame back between the poll and here, so the wait starts
                // over with the stop flag looked at again.
                continue;
            }
            if (got < static_cast<::ssize_t>(sizeof(fs_daemon_hdr)))
            {
                return;
            }
            keep(buffer);
        }
    }

    // A LOCK request is answered here and nowhere else. Anything of the ACCESS shape goes to the
    // case, which is what decides that one.
    void keep(const std::array<std::uint8_t, RequestFrameBytes>& buffer)
    {
        AccessRequestFrame_t frame;
        std::memcpy(&frame.hdr, buffer.data(), sizeof(frame.hdr));

        if (frame.hdr.type == static_cast<__u32>(FS_DAEMON_MSG_LOCK_REQUEST))
        {
            static_cast<void>(writeLockResponse(device_, frame.hdr.seq));
            return;
        }
        if (frame.hdr.type != static_cast<__u32>(FS_DAEMON_MSG_ACCESS_REQUEST) ||
            static_cast<std::size_t>(frame.hdr.payload_len) != sizeof(frame.payload))
        {
            return;
        }
        std::memcpy(&frame.payload, buffer.data() + sizeof(frame.hdr), sizeof(frame.payload));

        {
            const std::lock_guard<std::mutex> holding{guard_};
            waiting_.push_back(frame);
        }
        arrived_.notify_one();
    }

    std::int32_t device_;
    std::mutex guard_;
    std::condition_variable arrived_;
    std::deque<AccessRequestFrame_t> waiting_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

[[nodiscard]] std::string currentLogCutoff()
{
    const auto now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    return std::string{stamp};
}

// Every line dmesg has logged since @cutoff, or empty when the ring buffer could not be read.
[[nodiscard]] std::string dmesgSince(const std::string& cutoff)
{
    const std::string command = "dmesg --time-format=iso --since \"" + cutoff + "\" 2>/dev/null";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return {};
    }
    std::string collected;
    std::array<char, 512> chunk{};
    std::size_t got = 0;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), pipe)) > 0)
    {
        collected.append(chunk.data(), got);
    }
    static_cast<void>(::pclose(pipe));
    return collected;
}

// True when a WARNING or BUG appeared since @cutoff with "daemon" somewhere in its trace, which is
// the shape this module's own faults take rather than an unrelated one sharing the same window.
[[nodiscard]] bool logHasFreshModuleFault(const std::string& cutoff)
{
    std::istringstream lines{dmesgSince(cutoff)};
    std::vector<std::string> collected;
    std::string line;
    while (std::getline(lines, line))
    {
        collected.push_back(line);
    }

    constexpr std::size_t TraceWindow = 30;
    for (std::size_t index = 0; index < collected.size(); ++index)
    {
        const bool isFault = collected[index].find("WARNING:") != std::string::npos ||
                             collected[index].find("BUG:") != std::string::npos;
        if (!isFault)
        {
            continue;
        }
        const std::size_t windowEnd = std::min(collected.size(), index + TraceWindow);
        for (std::size_t trace = index; trace < windowEnd; ++trace)
        {
            if (collected[trace].find("daemon") != std::string::npos)
            {
                return true;
            }
        }
    }
    return false;
}

// A single ALLOW, then a second answer for the same request with DENY, deterministic and run once.
// The second write must not fail even though the kernel has nowhere left to put it. @file is
// already placed, since placing it here would ask the real helper for ATTEST with nobody left to
// answer that channel.
void checkSingleAnswerAcceptedOnce(Report& report, std::int32_t device, ChannelPump& pump,
                                   std::uint32_t node, fsuser::File& file)
{
    report.section("one request, answered twice");

    Baton result;
    if (!result.isOpen())
    {
        report.check("the parent and child can talk", false);
        return;
    }

    const ::pid_t child = ::fork();
    if (child == 0)
    {
        result.pass(fsuser::tests::probeReadMap(file.get(), PlacementSize));
        ::_exit(0);
    }
    report.check("fork succeeds", child > 0);
    if (child <= 0)
    {
        return;
    }

    AccessRequestFrame_t request;
    const bool got = pump.takeAccessRequest(request, AccessRequestWait);
    report.check("the fake helper reads the ACCESS request", got);
    if (!got)
    {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
        return;
    }

    report.check("the first answer, ALLOW, is written", writeAccessResponse(device, request.hdr.seq, true));

    const auto firstResult = result.take();
    report.checkErrno("the child's mapping is allowed by the first answer", firstResult == 0, firstResult);
    static_cast<void>(::waitpid(child, nullptr, 0));

    const bool secondWriteOk = writeAccessResponse(device, request.hdr.seq, false);
    report.check("a second answer to the same seq still writes cleanly", secondWriteOk);

    const auto depth = readTagged(readWholeLine(daemonStatePath(node).c_str()), "queue_depth=");
    report.check("the queue is already empty right after this one exchange",
                 depth.has_value() && *depth == 0);
}

// Runs one DENY-then-ALLOW pair back to back against @file, already placed for the same reason
// checkSingleAnswerAcceptedOnce takes one: creating it here would need the real helper for ATTEST.
// One placed file serves every attempt, since each fork is a fresh pid with no row of its own yet.
// Answers the attempt index at which the mapping was let through, or -1 when every one of
// ReversalAttempts stayed denied. -2 marks a run that could not be driven at all, which the caller
// reports on its own rather than folding into the count.
[[nodiscard]] std::int32_t findFirstReversal(Report& report, std::int32_t device, ChannelPump& pump,
                                             fsuser::File& file)
{
    for (std::int32_t attempt = 0; attempt < ReversalAttempts; ++attempt)
    {
        Baton result;
        if (!result.isOpen())
        {
            report.check("the parent and child can talk", false);
            return -2;
        }

        const ::pid_t child = ::fork();
        if (child == 0)
        {
            result.pass(fsuser::tests::probeReadMap(file.get(), PlacementSize));
            ::_exit(0);
        }
        if (child <= 0)
        {
            report.check("fork succeeds", false);
            return -2;
        }

        AccessRequestFrame_t request;
        const bool got = pump.takeAccessRequest(request, AccessRequestWait);
        if (!got)
        {
            report.check("the fake helper reads the ACCESS request", false);
            static_cast<void>(::kill(child, SIGKILL));
            static_cast<void>(::waitpid(child, nullptr, 0));
            return -2;
        }

        // Back to back, with nothing between them the kernel could reorder on.
        static_cast<void>(writeAccessResponse(device, request.hdr.seq, false));
        static_cast<void>(writeAccessResponse(device, request.hdr.seq, true));

        const auto mapped = result.take();
        static_cast<void>(::waitpid(child, nullptr, 0));

        if (mapped == 0)
        {
            return attempt;
        }
    }
    return -1;
}

void checkSecondAnswerCannotReverseTheFirst(Report& report, std::int32_t device, ChannelPump& pump,
                                            fsuser::File& file)
{
    report.section("a losing DENY raced against a winning ALLOW");

    const auto reversedAt = findFirstReversal(report, device, pump, file);
    if (reversedAt == -2)
    {
        return;
    }
    report.check("a DENY that answered first is never reversed by the ALLOW behind it",
                 reversedAt < 0);
    if (reversedAt >= 0)
    {
        report.note("attempt " + std::to_string(reversedAt) + " let the mapping through");
    }
}

// Everything that runs while the real helper is stopped and this case holds its channel. Its own
// function so the helper is back before the caller removes what it placed.
void driveTheStandInHelper(Report& report, std::uint32_t node, fsuser::File& onceFile,
                           fsuser::File& raceFile)
{
    const std::string unit = std::string{DAEMON_NAME_STR} + "@" + std::to_string(node) + ".service";
    const HelperServiceGuard guard{unit};
    report.check("the real helper stops", guard.stopped());
    if (!guard.stopped())
    {
        return;
    }

    const std::int32_t device = openHelperDevice(node);
    report.check("this case opens the channel in the real helper's place", device >= 0);
    if (device < 0)
    {
        return;
    }

    if (!sendHello(device))
    {
        report.check("HELLO is accepted", false);
        static_cast<void>(::close(device));
        return;
    }
    report.check("HELLO is accepted", true);

    // Nothing is queued on a channel just opened, and a read that waits for that to change is the
    // wait the pump below must never take. Asked before the pump exists, so nobody races it.
    ::pollfd quiet{device, POLLIN, 0};
    if (::poll(&quiet, 1, 0) == 0)
    {
        const std::int32_t emptied = readTheEmptyChannel(device);
        report.checkErrno("a read of the empty channel answers rather than waiting",
                          emptied == EAGAIN, emptied);
    }
    else
    {
        report.note("the channel already carried a request, so the empty read was not made");
    }

    const auto cutoff = currentLogCutoff();
    {
        ChannelPump pump{device};
        checkSingleAnswerAcceptedOnce(report, device, pump, node, onceFile);
        checkSecondAnswerCannotReverseTheFirst(report, device, pump, raceFile);
    }

    report.check("no fresh WARN or BUG from this module reached the log",
                 !logHasFreshModuleFault(cutoff));

    const auto depth = readTagged(readWholeLine(daemonStatePath(node).c_str()), "queue_depth=");
    report.check("the queue depth returns to zero with nothing left over",
                 depth.has_value() && *depth == 0);

    static_cast<void>(::close(device));
}

void checkNoSecondAnswerSticks(Report& report, fsuser::tests::Mount& mount, const char* nodeText)
{
    const auto node = static_cast<std::uint32_t>(std::atoi(nodeText));
    report.check("the node id parses", node > 0);
    if (node == 0)
    {
        return;
    }

    // Placed while the real helper still answers ATTEST: once the guard below takes the channel,
    // this case's stand-in only ever speaks ACCESS, and a placeFile here would wait on it forever.
    const auto onceName = caseName("security_double_answer_once");
    auto onceFile = fsuser::tests::placeFile(mount, onceName);
    const auto raceName = caseName("security_double_answer_race");
    auto raceFile = fsuser::tests::placeFile(mount, raceName);

    driveTheStandInHelper(report, node, onceFile, raceFile);

    // The real helper is back, so the turn each of these two writes needs has someone to answer it.
    try
    {
        mount.unlink(onceName);
        mount.unlink(raceName);
    }
    catch (const std::exception& failure)
    {
        report.raised("removing the regions this case placed", failure);
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

    const char* nodeText = std::getenv(FakeHelperNodeVariable);
    if (nodeText == nullptr)
    {
        std::printf(
            "test_security_double_answer: %s is unset, so this host was not asked to be "
            "driven\n",
            FakeHelperNodeVariable);
        return fsuser::tests::SkipStatus;
    }
    if (::geteuid() != 0)
    {
        std::printf("test_security_double_answer: needs root to stop the helper and open its channel\n");
        return fsuser::tests::SkipStatus;
    }
    if (!fsuser::tests::dropRealIdsToInvoker())
    {
        std::printf(
            "test_security_double_answer: SUDO_UID and SUDO_GID name the account the "
            "regions are placed under, and this run has neither\n");
        return fsuser::tests::SkipStatus;
    }
    Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkNoSecondAnswerSticks(report, mount, nodeText);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_double_answer", failure);
    }

    return report.summarise("test_security_double_answer");
}
