// SPDX-License-Identifier: Apache-2.0
//
// test_security_probe_gate -- what a channel admits when its helper has stopped answering.
//
// Two claims. A latched channel lets one probe through at a time, so callers arriving behind it are
// refused rather than queued to wait out the timeout each. A request of a kind the helper never
// declared in HELLO is refused at once instead of sitting on the queue until its wait runs out.
//
// The case stands in for the real helper, so it stops one for the run and refuses to drive anything
// unless told which node's it may take over. Each claim gets a freshly opened channel, because a
// latch left by one would decide the next.
//
// The region is placed before the stand-in takes over. A metadata write needs the peers' lock,
// which is an upcall of its own, so a channel held for another purpose could not serve one.
//
//   SECURITY_FAKE_HELPER_NODE=<node> test_security_probe_gate <mount-a>

#include <fcntl.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
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

// Names the node whose real helper this run may stop. Absent means this host has not agreed.
constexpr const char* FakeHelperNodeVariable = "SECURITY_FAKE_HELPER_NODE";

// An account this case borrows. Well clear of any the host itself hands out, so a row written under
// it names nobody who could then use it.
constexpr ::uid_t CallerUid = 60100;

// How many callers pile onto a latched channel at once, comfortably past the one a probe admits.
constexpr std::int32_t ProbeRacers = 12;

// Every latch cycle is one kernel wait long, so a short one is what lets the drive cover several.
constexpr std::uint32_t ShortWaitMs = 400;

// An order of magnitude under the kernel's own wait, so a refusal that took one is separated from
// an admission refused at once, with room for a loaded host.
constexpr std::int64_t PromptRefusalMs = 500;

constexpr std::int32_t SettleDeadlineMs = 8000;
constexpr std::int32_t SettlePollMs = 20;

// Room for the largest request shape the kernel sends, which a read needs before the header says
// which shape arrived.
constexpr std::size_t RequestFrameBytes = sizeof(fs_daemon_hdr) + sizeof(fs_daemon_access_request);

[[nodiscard]] std::string moduleParameterPath(const char* parameter)
{
    return std::string{"/sys/module/" FS_NAME_STR "/parameters/"} + parameter;
}

[[nodiscard]] bool writeDaemonTimeoutMs(std::uint32_t wanted)
{
    std::ofstream sink{moduleParameterPath("daemon_timeout_ms")};
    if (!sink)
    {
        return false;
    }
    sink << wanted;
    return sink.good();
}

[[nodiscard]] std::uint32_t readDaemonTimeoutMs()
{
    const auto text = readWholeLine(moduleParameterPath("daemon_timeout_ms").c_str());
    if (text.empty())
    {
        return 0;
    }
    try
    {
        return static_cast<std::uint32_t>(std::stoul(text));
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

// Puts the kernel's wait back whatever way the section it guards ends.
class DaemonTimeoutGuard
{
public:
    explicit DaemonTimeoutGuard(std::uint32_t wanted)
        : before_{readDaemonTimeoutMs()},
          applied_{writeDaemonTimeoutMs(wanted)}
    {
    }

    ~DaemonTimeoutGuard()
    {
        if (applied_)
        {
            static_cast<void>(writeDaemonTimeoutMs(before_));
        }
    }

    DaemonTimeoutGuard(const DaemonTimeoutGuard&) = delete;
    DaemonTimeoutGuard& operator=(const DaemonTimeoutGuard&) = delete;

    [[nodiscard]] bool applied() const noexcept
    {
        return applied_;
    }

private:
    std::uint32_t before_;
    bool applied_;
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
            std::fprintf(stderr, "test_security_probe_gate: could not restart %s\n", unit_.c_str());
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

// The one handshake a channel needs before it queues anything, declaring @caps and no more. What is
// left out is what the kernel then has to refuse rather than send.
[[nodiscard]] bool sendHello(std::int32_t device, std::uint64_t caps)
{
    fs_daemon_hdr hdr{};
    hdr.magic = FS_DAEMON_MAGIC;
    hdr.version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hdr.type = static_cast<__u32>(FS_DAEMON_MSG_HELLO);
    hdr.payload_len = static_cast<__u32>(sizeof(fs_daemon_hello));

    fs_daemon_hello hello{};
    hello.protocol_version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
    hello.capabilities = caps;
    hello.helper_pid = static_cast<__u32>(::getpid());

    std::array<std::uint8_t, sizeof(hdr) + sizeof(hello)> frame{};
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &hello, sizeof(hello));

    return ::write(device, frame.data(), frame.size()) == static_cast<::ssize_t>(frame.size());
}

// Holds the channel open for one claim and drops it again, which purges whatever is still queued on
// it and lets every caller this case parked there return.
class FakeHelper
{
public:
    FakeHelper(std::uint32_t node, std::uint64_t caps)
        : device_{openHelperDevice(node)},
          greeted_{device_ >= 0 && sendHello(device_, caps)}
    {
    }

    ~FakeHelper()
    {
        release();
    }

    FakeHelper(const FakeHelper&) = delete;
    FakeHelper& operator=(const FakeHelper&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return greeted_;
    }

    [[nodiscard]] std::int32_t get() const noexcept
    {
        return device_;
    }

    void release()
    {
        if (device_ >= 0)
        {
            static_cast<void>(::close(device_));
            device_ = -1;
        }
    }

private:
    std::int32_t device_;
    bool greeted_;
};

// How long a serving thread sits in one poll before it looks at whether it was asked to stop.
constexpr std::int32_t ServePollMs = 100;

// Answers LOCK with the turn until @stopping is set, counting every frame it was handed. It waits
// in poll rather than in read, since closing the channel never wakes a thread already inside read.
void serveLocks(std::int32_t device, std::atomic<std::int32_t>& served, std::atomic<bool>& stopping)
{
    std::array<std::uint8_t, RequestFrameBytes> buffer{};
    while (!stopping.load())
    {
        ::pollfd watched{device, POLLIN, 0};
        const auto ready = ::poll(&watched, 1, ServePollMs);
        if (ready < 0)
        {
            return;
        }
        if (ready == 0)
        {
            continue;
        }

        const auto got = ::read(device, buffer.data(), buffer.size());
        if (got < 0 && (errno == EAGAIN || errno == EINTR))
        {
            // The requester took its frame back between the poll and here, so the wait starts over
            // with the stop flag looked at again.
            continue;
        }
        if (got < static_cast<::ssize_t>(sizeof(fs_daemon_hdr)))
        {
            return;
        }

        fs_daemon_hdr request{};
        std::memcpy(&request, buffer.data(), sizeof(request));
        served.fetch_add(1);
        if (request.type != static_cast<__u32>(FS_DAEMON_MSG_LOCK_REQUEST))
        {
            continue;
        }

        fs_daemon_hdr hdr{};
        hdr.magic = FS_DAEMON_MAGIC;
        hdr.version = static_cast<__u32>(FS_DAEMON_PROTOCOL_MAX);
        hdr.type = static_cast<__u32>(FS_DAEMON_MSG_LOCK_RESPONSE);
        hdr.payload_len = static_cast<__u32>(sizeof(fs_daemon_lock_response));
        hdr.seq = request.seq;

        fs_daemon_lock_response resp{};
        resp.status = 0;

        std::array<std::uint8_t, sizeof(hdr) + sizeof(resp)> reply{};
        std::memcpy(reply.data(), &hdr, sizeof(hdr));
        std::memcpy(reply.data() + sizeof(hdr), &resp, sizeof(resp));
        if (::write(device, reply.data(), reply.size()) != static_cast<::ssize_t>(reply.size()))
        {
            return;
        }
    }
}

[[nodiscard]] std::uint64_t readChannelField(std::uint32_t node, const char* field)
{
    const auto found = readTagged(readWholeLine(daemonStatePath(node).c_str()), field);
    return found.value_or(0);
}

[[nodiscard]] std::uint64_t readQueueDepth(std::uint32_t node)
{
    return readChannelField(node, "queue_depth=");
}

[[nodiscard]] bool waitForLatch(std::uint32_t node)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(SettleDeadlineMs);
    while (std::chrono::steady_clock::now() < until)
    {
        if (readChannelField(node, "helper_dead=") == 1)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(SettlePollMs));
    }
    return false;
}

// One caller of the region, run as @uid so the kernel judges it as that account. The descriptor is
// the parent's, opened before the account changed, so what the module judges is the mapping alone.
[[nodiscard]] ::pid_t forkOneCaller(std::int32_t descriptor, ::uid_t uid)
{
    const ::pid_t child = ::fork();
    if (child != 0)
    {
        return child;
    }

    if (::setregid(uid, uid) != 0 || ::setreuid(uid, uid) != 0)
    {
        ::_exit(3);
    }
    ::_exit(fsuser::tests::probeReadMap(descriptor, PlacementSize) == 0 ? 0 : 1);
}

// What one caller's mapping answered, run as @uid. This case owns the region, so a mapping it made
// itself would be the owner's own and never reach the admission a claim about one is asking about.
[[nodiscard]] std::int32_t mapAsCaller(std::int32_t descriptor, ::uid_t uid)
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
        if (::setregid(uid, uid) != 0 || ::setreuid(uid, uid) != 0)
        {
            outcome.pass(EPERM);
            ::_exit(3);
        }
        outcome.pass(fsuser::tests::probeReadMap(descriptor, PlacementSize));
        ::_exit(0);
    }

    const std::int32_t answered = outcome.take();
    static_cast<void>(::waitpid(child, nullptr, 0));
    return answered;
}

void reapAll(std::vector<::pid_t>& children)
{
    for (const auto child : children)
    {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, nullptr, 0));
    }
    children.clear();
}

// How many of @children have already finished, without waiting for the rest. A caller the kernel
// turned away returns at once, where one it queued is still parked on its answer.
[[nodiscard]] std::int32_t countFinished(const std::vector<::pid_t>& children)
{
    std::int32_t finished = 0;
    for (const auto child : children)
    {
        if (::waitpid(child, nullptr, WNOHANG) == child)
        {
            ++finished;
        }
    }
    return finished;
}

// ── Claim 1: one probe at a time ─────────────────────────────────────────

void checkLatchAdmitsOneProbe(Report& report, fsuser::File& file, std::uint32_t node)
{
    report.section("what a latched channel admits");

    const DaemonTimeoutGuard wait{ShortWaitMs};
    if (!wait.applied())
    {
        report.note("this module carries no test knobs, so the kernel's own wait is not shortened");
    }
    const std::int32_t driveMs = wait.applied() ? 4000 : 16000;

    const FakeHelper helper{node, static_cast<std::uint64_t>(FS_DAEMON_CAP_ACCESS_REQUEST)};
    report.check("the channel opens in the real helper's place", helper.ready());
    if (!helper.ready())
    {
        return;
    }

    // Nothing here reads the device, so the one caller below gets no answer and the channel latches.
    std::vector<::pid_t> starter{forkOneCaller(file.get(), CallerUid)};
    report.check("the channel latches on a request nobody answers", waitForLatch(node));

    std::atomic<std::uint64_t> deepest{0};
    std::atomic<bool> driving{true};

    std::thread sampler{[&]
                        {
                            while (driving.load())
                            {
                                const auto depth = readQueueDepth(node);
                                if (depth > deepest.load())
                                {
                                    deepest.store(depth);
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            }
                        }};

    // Forked afresh each round, because one caller's request ends when its wait does.
    std::int32_t racersRun = 0;
    std::int32_t refusedAtOnce = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(driveMs);
    while (std::chrono::steady_clock::now() < until)
    {
        std::vector<::pid_t> racers;
        for (std::int32_t racer = 0; racer < ProbeRacers; ++racer)
        {
            const ::pid_t child = forkOneCaller(file.get(), CallerUid);
            if (child > 0)
            {
                racers.push_back(child);
            }
        }
        racersRun += static_cast<std::int32_t>(racers.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(ShortWaitMs / 4));
        refusedAtOnce += countFinished(racers);
        reapAll(racers);
    }

    driving.store(false);
    sampler.join();

    const auto peak = deepest.load();
    report.check("the latch let a probe through at all: peak depth " + std::to_string(peak), peak >= 1);
    report.check("no more than that one probe is ever in flight: peak depth " + std::to_string(peak),
                 peak <= 1);
    report.check("the rest are refused without waiting: " + std::to_string(refusedAtOnce) + " of " +
                     std::to_string(racersRun),
                 racersRun > 0 && refusedAtOnce > racersRun / 2);

    reapAll(starter);
}

// ── Claim 2: the capability a helper declared ────────────────────────────

void checkUndeclaredKindIsRefused(Report& report, fsuser::File& file, std::uint32_t node)
{
    report.section("a request of a kind the helper never declared");

    // LOCK and no more, so a mapping's ACCESS is the one kind this helper cannot answer.
    FakeHelper helper{node, static_cast<std::uint64_t>(FS_DAEMON_CAP_LOCK)};
    report.check("the channel opens in the real helper's place", helper.ready());
    if (!helper.ready())
    {
        return;
    }

    std::atomic<std::int32_t> served{0};
    std::atomic<bool> stopping{false};
    std::thread responder{serveLocks, helper.get(), std::ref(served), std::ref(stopping)};

    const auto beganAt = std::chrono::steady_clock::now();
    const auto outcome = mapAsCaller(file.get(), CallerUid);
    const auto tookMs = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - beganAt)
            .count());

    report.checkErrno("a mapping is refused by a helper that declared no ACCESS", outcome != 0, outcome);
    report.check("the refusal costs no wait at all: " + std::to_string(tookMs) + " ms",
                 tookMs < PromptRefusalMs);
    report.check("nothing of that kind was ever queued: depth " + std::to_string(readQueueDepth(node)),
                 readQueueDepth(node) == 0);
    report.check("the helper was handed nothing it never declared: " + std::to_string(served.load()),
                 served.load() == 0);

    stopping.store(true);
    responder.join();
    helper.release();
}

void checkAnUnansweredChannelHoldsTheLine(Report& report, fsuser::tests::Mount& mount,
                                          std::uint32_t node)
{
    // Placed while the real helper still serves, because a metadata write needs the peers' lock and
    // the stand-in below is holding the channel that would carry it.
    const auto name = caseName("probe_gate");
    auto file = placeFile(mount, name);

    const std::string unit = std::string{DAEMON_NAME_STR} + "@" + std::to_string(node) + ".service";
    {
        const HelperServiceGuard guard{unit};
        report.check("the real helper stops", guard.stopped());
        if (guard.stopped())
        {
            checkLatchAdmitsOneProbe(report, file, node);
            checkUndeclaredKindIsRefused(report, file, node);
        }
    }

    // The real helper is back, so the metadata write this needs has something to answer it.
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

    const char* nodeText = std::getenv(FakeHelperNodeVariable);
    if (nodeText == nullptr)
    {
        std::printf("test_security_probe_gate: %s is unset, so this host was not asked to be driven\n",
                    FakeHelperNodeVariable);
        return SkipStatus;
    }
    if (::geteuid() != 0)
    {
        std::printf("test_security_probe_gate: needs root to stop the helper and open its channel\n");
        return SkipStatus;
    }
    const auto node = static_cast<std::uint32_t>(std::atoi(nodeText));
    if (node == 0)
    {
        std::printf("test_security_probe_gate: %s does not name a node\n", FakeHelperNodeVariable);
        return 2;
    }
    if (!fsuser::tests::dropRealIdsToInvoker())
    {
        std::printf(
            "test_security_probe_gate: SUDO_UID and SUDO_GID name the account the region "
            "is placed under, and this run has neither\n");
        return SkipStatus;
    }

    Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkAnUnansweredChannelHoldsTheLine(report, mount, node);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_probe_gate", failure);
    }

    return report.summarise("test_security_probe_gate");
}
