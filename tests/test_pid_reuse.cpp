// SPDX-License-Identifier: Apache-2.0
//
// test_pid_reuse -- a delegation row names one process by pid and start time, so the next holder of
// a dead process's pid is judged fresh rather than inheriting its row. This case hands one pid out
// twice by steering ns_last_pid (CAP_SYS_ADMIN) or, lacking that, by lapping the pid space under
// <FS>_TEST_PID_LAP, then reads deleg_info to tell a fresh grant from an inherited one. Needs root.
//
//   test_pid_reuse <mount-a> <mount-b>
//   <FS>_TEST_PID_LAP=1 test_pid_reuse <mount-a> <mount-b>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/probe.hpp"
#include "harness/slot.hpp"
#include "harness/sysfs.hpp"
#include "name.h"

namespace
{

// A delegation row is matched only from the node it names, so the children reach the file through
// the peer's mount and earn their rows there.

// How many times a wanted pid is asked for before the host is called too busy. Any process may take
// the steered value first, and retrying is what makes a busy host a slower run rather than a red one.
constexpr std::int32_t PidAttempts = 8;

// How many processes consume pids during a lap. Enough that the lap costs about a minute rather than
// eight, few enough to leave the host to its own work.
constexpr std::int32_t LapBurners = 16;

// How close the counter must come before the burners stop. Each of them takes a few more pids while
// it winds down, and the single steps that follow cover the rest.
constexpr std::int64_t LapApproach = 30000;

// A lap that has not come round by here is a lap something else is interfering with.
constexpr std::int64_t LapDeadlineSeconds = 900;
constexpr std::int64_t LapPollMillis = 100;

constexpr const char* LastPidPath = "/proc/sys/kernel/ns_last_pid";
constexpr const char* PidCeilingPath = "/proc/sys/kernel/pid_max";
constexpr const char* LapEnv = FS_NAME_UPPER_STR "_TEST_PID_LAP";

// Root only, since the rows it names carry other accounts' uids.
constexpr const char* DelegInfoPath = "/sys/fs/" FS_NAME_STR "/deleg_info";

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// Which of the two ways this run has for handing one pid out twice.
enum class Reach : std::int32_t
{
    Nothing,
    Steer,
    Lap,
};

enum class Outcome : std::int32_t
{
    Measured,
    PidNotReused,
    StaleRowGone,
};

[[nodiscard]] bool writeLastPid(const std::string& text)
{
    const std::int32_t writing = ::open(LastPidPath, O_WRONLY | O_CLOEXEC);
    if (writing < 0)
    {
        return false;
    }
    const std::int64_t put = ::write(writing, text.data(), text.size());
    ::close(writing);
    return put == static_cast<std::int64_t>(text.size());
}

// The sysctl needs CAP_SYS_ADMIN to write, so a probe write is the only way to ask. Putting back the
// value it already holds steers nothing, since the next fork takes the one after it either way.
[[nodiscard]] bool canSteerPid()
{
    const std::int32_t reading = ::open(LastPidPath, O_RDONLY | O_CLOEXEC);
    if (reading < 0)
    {
        return false;
    }
    char held[32] = {};
    const std::int64_t got = ::read(reading, held, sizeof(held) - 1);
    ::close(reading);
    if (got <= 0)
    {
        return false;
    }
    return writeLastPid(std::string{held, static_cast<std::size_t>(got)});
}

// The kernel allocates the value after the one this holds, so naming one less asks for @wanted.
[[nodiscard]] bool steerNextPid(::pid_t wanted)
{
    return writeLastPid(std::to_string(wanted - 1));
}

// Which way this run can hand one pid out twice, in the order that costs least.
[[nodiscard]] Reach reachAvailable()
{
    if (canSteerPid())
    {
        return Reach::Steer;
    }
    return (::getenv(LapEnv) != nullptr) ? Reach::Lap : Reach::Nothing;
}

// ── the lap ─────────────────────────────────────────────────────────────

[[nodiscard]] std::int64_t readNumber(const char* path)
{
    const std::int32_t reading = ::open(path, O_RDONLY | O_CLOEXEC);
    if (reading < 0)
    {
        return -1;
    }
    char held[32] = {};
    const std::int64_t got = ::read(reading, held, sizeof(held) - 1);
    ::close(reading);
    return (got > 0) ? std::strtoll(held, nullptr, 10) : -1;
}

// Runs in a burner. Takes pids and gives them straight back, until the parent kills it.
[[noreturn]] void burnUntilKilled()
{
    for (;;)
    {
        const ::pid_t child = ::fork();
        if (child == 0)
        {
            ::_exit(0);
        }
        if (child < 0)
        {
            // The table is momentarily full, so let the others drain it.
            ::usleep(1000);
            continue;
        }
        static_cast<void>(::waitpid(child, nullptr, 0));
    }
}

// Consumes pids until the counter has passed pid_max, come round, and drawn near @wanted. Answers
// false when it never got there, which is a host busy enough that the lap says nothing.
[[nodiscard]] bool burnUntilNear(::pid_t wanted)
{
    std::vector<::pid_t> crew;
    for (std::int32_t index = 0; index < LapBurners; ++index)
    {
        const ::pid_t one = ::fork();
        if (one == 0)
        {
            burnUntilKilled();
        }
        if (one > 0)
        {
            crew.push_back(one);
        }
    }

    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(LapDeadlineSeconds);
    bool wrapped = false;
    bool arrived = false;
    std::int64_t seen = readNumber(LastPidPath);

    while (std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(LapPollMillis));
        const std::int64_t now = readNumber(LastPidPath);
        if (now < 0)
        {
            break;
        }
        // The counter only falls when it has passed the ceiling and started again.
        wrapped = wrapped || (now < seen);
        seen = now;
        if (wrapped && now >= static_cast<std::int64_t>(wanted) - LapApproach)
        {
            arrived = now < static_cast<std::int64_t>(wanted);
            break;
        }
    }

    for (const ::pid_t one : crew)
    {
        ::kill(one, SIGKILL);
    }
    for (const ::pid_t one : crew)
    {
        static_cast<void>(::waitpid(one, nullptr, 0));
    }
    return arrived;
}

// Runs in a child. Waits for the first begin, opens the file as the peer node and maps it, answers
// the errno the kernel gave (zero means it mapped), then stays alive until the second begin so the
// parent reads its row while the process still exists.
[[noreturn]] void mapAsPeerAndHold(fsuser::tests::Handoff_t& handoff, const std::string& mount,
                                   const std::string& name)
{
    static_cast<void>(handoff.begin.take());
    handoff.ack.pass(fsuser::tests::openAndProbeReadMap(mount, name, PlacementSize));
    static_cast<void>(handoff.begin.take());
    ::_exit(0);
}

void reap(::pid_t child)
{
    static_cast<void>(::waitpid(child, nullptr, 0));
}

// The child that the grant names. It waits on the first begin because the grant is written after the
// fork, which is the only order available: the row names a pid nobody knows before the fork returns.
[[nodiscard]] ::pid_t forkGrantedChild(fsuser::tests::Handoff_t& handoff, const std::string& mount,
                                       const std::string& name)
{
    const ::pid_t child = ::fork();
    if (child == 0)
    {
        mapAsPeerAndHold(handoff, mount, name);
    }
    return child;
}

// Forks one at a time until a child comes back holding @wanted. A child that missed it exits at once
// without touching the mount, so only the wanted pid's fork earns a row. The wanted child
// waits on @handoff before it maps.
[[nodiscard]] ::pid_t stepUntilPidReused(Reach reach, ::pid_t wanted, fsuser::tests::Handoff_t& handoff,
                                         const std::string& mount, const std::string& name)
{
    const std::int64_t rounds = (reach == Reach::Lap) ? LapApproach * 4 : PidAttempts;
    for (std::int64_t attempt = 0; attempt < rounds; ++attempt)
    {
        if (reach == Reach::Steer && !steerNextPid(wanted))
        {
            return -1;
        }
        const ::pid_t child = ::fork();
        if (child == 0)
        {
            if (::getpid() == wanted)
            {
                mapAsPeerAndHold(handoff, mount, name);
            }
            ::_exit(0);
        }
        if (child == wanted)
        {
            return child;
        }
        if (child > 0)
        {
            reap(child);
        }
        // Stepping is one-way, so a value past the wanted one means somebody else took it.
        if (reach == Reach::Lap && child > wanted)
        {
            return -1;
        }
    }
    return -1;
}

// Hands @wanted out a second time by whichever route this run has.
[[nodiscard]] ::pid_t reachPidAgain(Reach reach, ::pid_t wanted, fsuser::tests::Handoff_t& handoff,
                                    const std::string& mount, const std::string& name)
{
    if (reach == Reach::Lap && !burnUntilNear(wanted))
    {
        return -1;
    }
    return stepUntilPidReused(reach, wanted, handoff, mount, name);
}

// The birth_time deleg_info carries on each process row of @ratEntry naming @wanted, from a write
// that selects the region and the read right after it. Root only, like the file itself.
[[nodiscard]] std::vector<std::uint64_t> processRowBirths(std::int64_t ratEntry, ::pid_t wanted)
{
    std::vector<std::uint64_t> births;
    {
        std::ofstream select{DelegInfoPath};
        if (!select)
        {
            return births;
        }
        select << ratEntry << "\n";
    }
    std::ifstream source{DelegInfoPath};
    std::string line;
    while (std::getline(source, line))
    {
        if (line.find("deleg[") == std::string::npos || line.find(" process ") == std::string::npos)
        {
            continue;
        }
        const auto rowPid = fsuser::tests::readTagged(line, "pid=");
        const auto rowBirth = fsuser::tests::readTagged(line, "birth_time=");
        if (rowPid && rowBirth && static_cast<::pid_t>(*rowPid) == wanted)
        {
            births.push_back(*rowBirth);
        }
    }
    return births;
}

Outcome checkPidReuse(fsuser::tests::Report& report, Reach reach, fsuser::tests::Mount& mount,
                      const std::string& peerMount, const std::string& name)
{
    report.section("a delegation and the process that inherits its pid");

    auto file = fsuser::tests::placeFile(mount, name);
    const auto row = fsuser::tests::findRowByName(name);
    report.check("region_info carries a row for the new name", row.has_value());
    if (!row)
    {
        return Outcome::Measured;
    }

    fsuser::tests::Handoff_t grantedHandoff;
    fsuser::tests::Handoff_t reusedHandoff;
    report.check("the handoff pipes open", grantedHandoff.isOpen() && reusedHandoff.isOpen());

    const ::pid_t granted = forkGrantedChild(grantedHandoff, peerMount, name);
    report.check("fork succeeds", granted > 0);
    if (granted <= 0)
    {
        return Outcome::Measured;
    }

    // No grant from here. The child's map is refused on the record, the helper on its node
    // decides, and the row the kernel then writes names that child and its start time.
    grantedHandoff.begin.pass(1);
    const std::int32_t bound = grantedHandoff.ack.take();
    report.checkErrno("the granted child maps and so binds its start time", bound == 0, bound);

    // The child is alive here, so the sweep cannot have taken its row.
    const auto grantedBirths = processRowBirths(row->entry, granted);
    report.check("the granted child's row is recorded with a birth time", grantedBirths.size() == 1);
    grantedHandoff.begin.pass(1);
    reap(granted);
    if (grantedBirths.empty())
    {
        return Outcome::Measured;
    }
    const std::uint64_t grantedBirth = grantedBirths.front();

    const ::pid_t reused = reachPidAgain(reach, granted, reusedHandoff, peerMount, name);
    if (reused < 0)
    {
        return Outcome::PidNotReused;
    }

    // The sweep may take the dead child's row at any time. Unless it is still there when the pid
    // comes back, a fresh row proves nothing about rejecting a stale one.
    const auto staleBirths = processRowBirths(row->entry, granted);
    const bool staleStill =
        std::find(staleBirths.begin(), staleBirths.end(), grantedBirth) != staleBirths.end();
    reusedHandoff.begin.pass(1);
    const std::int32_t left = reusedHandoff.ack.take();
    if (left == 0)
    {
        const auto reusedBirths = processRowBirths(row->entry, reused);
        reusedHandoff.begin.pass(1);
        reap(reused);
        if (!staleStill)
        {
            return Outcome::StaleRowGone;
        }
        const bool freshRow = std::any_of(reusedBirths.begin(), reusedBirths.end(),
                                          [grantedBirth](std::uint64_t birth)
                                          {
                                              return birth != grantedBirth;
                                          });
        report.check(
            "the next holder of that pid maps under a fresh row and not the dead one", freshRow);
        return Outcome::Measured;
    }
    reusedHandoff.begin.pass(1);
    reap(reused);
    report.checkErrno("the next holder of that pid gets its own verdict and is refused",
                      left == EACCES, left);
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

    // deleg_info is root's, and it is what separates a fresh grant from an inherited row.
    if (::geteuid() != 0)
    {
        std::printf("test_pid_reuse: not root, so deleg_info cannot be read: skipping\n");
        return fsuser::tests::SkipStatus;
    }
    // The kernel attests the real uid, which the daemon's rules name as the invoking account.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    const Reach reach = reachAvailable();
    if (reach == Reach::Nothing)
    {
        std::printf("test_pid_reuse: skipped, steering a pid needs CAP_SYS_ADMIN and %s is unset\n",
                    LapEnv);
        return fsuser::tests::SkipStatus;
    }
    if (reach == Reach::Lap)
    {
        std::printf("test_pid_reuse: no capability, so taking a lap of all %lld pids\n",
                    static_cast<long long>(readNumber(PidCeilingPath)));
        std::fflush(stdout);
    }

    fsuser::tests::Report report;
    Outcome outcome = Outcome::Measured;
    const auto name = caseName("pid_reuse");

    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        outcome = checkPidReuse(report, reach, mount, mounts.second(), name);
        mount.unlink(name);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_pid_reuse setup", failure);
    }

    if (outcome == Outcome::PidNotReused && !report.anyFailed())
    {
        std::printf("test_pid_reuse: skipped, another process took the pid first\n");
        return fsuser::tests::SkipStatus;
    }
    if (outcome == Outcome::StaleRowGone && !report.anyFailed())
    {
        std::printf("test_pid_reuse: skipped, the dead child's row was swept before its pid came back\n");
        return fsuser::tests::SkipStatus;
    }
    return report.summarise("test_pid_reuse");
}
