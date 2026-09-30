// SPDX-License-Identifier: Apache-2.0
//
// probe_ops -- how long one metadata operation takes on one mount.
//
// A phase runs its call over its own set of names, so a sample waits on no sample before it except
// through the shared turn. The turn counter is read around each phase, so the number of turns a
// phase took is measured rather than inferred from its timing.
//
// Not a case. It asserts nothing and returns 0 whatever it measures, which is why this directory
// builds it without registering it: a slow run here is a result and not a failure.
//
// The timed calls are the syscalls rather than libroof, because what this prices is the round trip
// the kernel makes to a helper, and a library wrapper inside the window would be priced with it.
//
// The names carry the tag rather than the pid, so one run lays the files down and a later run times
// a single phase over them with nothing of its own inside the window a reader samples.
//
// It runs on any filesystem, which is what makes a baseline possible: a mount that reports no
// node_id is one this module does not own, and the turn column then reads 0 for every phase.
//
// Every open carries O_CLOEXEC, because this filesystem refuses to map a descriptor without it and
// a baseline that opened differently would not be pricing the same call.
//
//   probe_ops <mount> <rounds> <tag> [create|place|mmap|fault|lookup|unlink|make|all]
//
// PROBE_GAP_US in the environment idles that long before each call that takes a turn, so a run can
// price the turn against a helper that has gone quiet as well as against one still finishing the
// release before it. Unset or 0 is back to back.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/stats.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::MetaTurns_t;
using fsuser::tests::Mount;
using fsuser::tests::PlacementSize;
using fsuser::tests::readMetaTurns;

using Clock = std::chrono::steady_clock;

// Below four a percentile lands on the same sample twice, which reads as a spread that is not there.
constexpr std::uint32_t LeastRounds = 4;

// What one run was asked for. Every phase below reads these and none changes any, which is what
// puts them in one type rather than in each signature.
struct Run_t
{
    std::string_view tag;
    std::string_view only;
    std::uint32_t node{0};
    std::uint32_t gapUs{0};
};

// One name and what the phases did with it. Holding the three together is what keeps every loop
// below a range and every index out of them.
struct Round_t
{
    std::string name;
    std::int32_t handle{-1};
    void* mapped{nullptr};
    double took{0.0};
};

[[nodiscard]] double takeElapsedUs(Clock::time_point began, Clock::time_point ended)
{
    return std::chrono::duration<double, std::micro>{ended - began}.count();
}

// Spun rather than slept: a sleep's own wakeup would land inside the window the next call is timed
// in, and a few tens of microseconds is below what the scheduler resolves.
void idleFor(std::uint32_t gapUs)
{
    if (gapUs == 0)
    {
        return;
    }
    const auto until = Clock::now() + std::chrono::microseconds{gapUs};
    while (Clock::now() < until)
    {
    }
}

// The node id the mount reports, so the turn counter read around a phase is that mount's own. A
// case names its nodes as constants instead, because the suite hands it the mounts in order.
[[nodiscard]] std::int64_t findNodeOfMount(const std::string& point)
{
    std::ifstream mounts{"/proc/self/mounts"};
    std::string line;
    while (std::getline(mounts, line))
    {
        if (line.find(' ' + point + ' ') == std::string::npos)
        {
            continue;
        }
        const auto marker = line.find("node_id=");
        if (marker == std::string::npos)
        {
            return -1;
        }
        return std::stoll(line.substr(marker + std::strlen("node_id=")));
    }
    return -1;
}

[[nodiscard]] std::uint64_t readTurnsTaken(std::uint32_t nodeId)
{
    const auto turns = readMetaTurns(nodeId);
    return turns ? turns->taken : 0;
}

// One CSV row per phase, so a driver collects several runs without parsing prose.
void printPhase(std::string_view tag, std::string_view phase, const std::vector<Round_t>& rounds,
                std::uint64_t turns)
{
    std::vector<double> samples;
    samples.reserve(rounds.size());
    for (const auto& round : rounds)
    {
        samples.push_back(round.took);
    }
    const auto summary = fsuser::tests::summarise(samples);
    if (!summary)
    {
        std::printf("%s,%s,0,,,,,,%llu\n", std::string{tag}.c_str(), std::string{phase}.c_str(),
                    static_cast<unsigned long long>(turns));
        return;
    }
    std::printf("%s,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%llu\n", std::string{tag}.c_str(),
                std::string{phase}.c_str(), static_cast<unsigned long long>(summary->count),
                summary->least, summary->median, summary->p95, summary->most, summary->mean,
                static_cast<unsigned long long>(turns));
    std::fflush(stdout);
}

[[nodiscard]] bool wantsPhase(std::string_view phase, std::string_view only)
{
    return only == "all" || only == phase;
}

// create: one turn for the name. The handles stay open for the phase after it.
[[nodiscard]] bool runCreate(std::vector<Round_t>& rounds, std::uint32_t gapUs)
{
    for (auto& round : rounds)
    {
        idleFor(gapUs);
        const auto began = Clock::now();
        round.handle = ::open(round.name.c_str(), O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0644);
        round.took = takeElapsedUs(began, Clock::now());
        if (round.handle < 0)
        {
            std::perror("open O_CREAT");
            return false;
        }
    }
    return true;
}

// place: the extent the truncate reserves is what this turn protects.
[[nodiscard]] bool runPlace(std::vector<Round_t>& rounds, std::uint32_t gapUs)
{
    for (auto& round : rounds)
    {
        idleFor(gapUs);
        const auto began = Clock::now();
        const auto failed = ::ftruncate(round.handle, static_cast<::off_t>(PlacementSize));
        round.took = takeElapsedUs(began, Clock::now());
        if (failed != 0)
        {
            std::perror("ftruncate");
            return false;
        }
    }
    return true;
}

// mmap: the data path. On this filesystem a mapping that finds no grant earns one through an
// upcall, so what this times is that decision and not the fault.
[[nodiscard]] bool runMmap(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        const auto began = Clock::now();
        round.mapped = ::mmap(nullptr, PlacementSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                              round.handle, 0);
        round.took = takeElapsedUs(began, Clock::now());
        if (round.mapped == MAP_FAILED)
        {
            round.mapped = nullptr;
            std::perror("mmap");
            return false;
        }
    }
    return true;
}

// fault: the first access to a mapping already standing, which is the page the fault path installs.
void runFault(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        const auto* first = static_cast<const volatile unsigned char*>(round.mapped);
        const auto began = Clock::now();
        const auto seen = *first;
        round.took = takeElapsedUs(began, Clock::now());
        (void)seen;
    }
}

// lookup: no turn, so this is the syscall floor the other phases sit on.
void runLookup(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        const auto began = Clock::now();
        const auto opened = ::open(round.name.c_str(), O_RDONLY | O_CLOEXEC);
        round.took = takeElapsedUs(began, Clock::now());
        if (opened >= 0)
        {
            ::close(opened);
        }
    }
}

void runUnlink(std::vector<Round_t>& rounds, std::uint32_t gapUs)
{
    for (auto& round : rounds)
    {
        idleFor(gapUs);
        const auto began = Clock::now();
        const auto failed = ::unlink(round.name.c_str());
        round.took = takeElapsedUs(began, Clock::now());
        if (failed != 0)
        {
            std::perror("unlink");
        }
    }
}

// What every phase does the same way: read the turn counter, run the calls, read it again, and
// print only when this run asked for that phase. @phase answers false when a call failed.
template <typename T_Phase>
[[nodiscard]] bool takePhase(std::string_view name, const Run_t& run,
                             std::vector<Round_t>& rounds, T_Phase&& phase)
{
    const auto before = readTurnsTaken(run.node);
    if (!phase(rounds))
    {
        return false;
    }
    if (wantsPhase(name, run.only))
    {
        printPhase(run.tag, name, rounds, readTurnsTaken(run.node) - before);
    }
    return true;
}

// The names: create lays them down and place gives each an extent. "make" runs both and prints
// neither, so a later run times one call over files that are already there.
[[nodiscard]] bool takeNamePhases(const Run_t& run, std::vector<Round_t>& rounds)
{
    const auto places = wantsPhase("place", run.only) || run.only == "make";
    const auto spaced = [&run](auto&& call)
    {
        return [&run, call](std::vector<Round_t>& each)
        {
            return call(each, run.gapUs);
        };
    };
    if (wantsPhase("create", run.only) || places)
    {
        if (!takePhase("create", run, rounds, spaced(runCreate)))
        {
            return false;
        }
    }
    if (places && !takePhase("place", run, rounds, spaced(runPlace)))
    {
        return false;
    }
    return true;
}

// The mapping: fault needs a mapping already standing, so mmap runs whenever either is asked for.
[[nodiscard]] bool takeMapPhases(const Run_t& run, std::vector<Round_t>& rounds)
{
    if (!wantsPhase("mmap", run.only) && !wantsPhase("fault", run.only))
    {
        return true;
    }
    if (!takePhase("mmap", run, rounds, runMmap))
    {
        return false;
    }
    if (wantsPhase("fault", run.only))
    {
        return takePhase("fault", run, rounds, [](std::vector<Round_t>& each)
                         {
                             runFault(each);
                             return true;
                         });
    }
    return true;
}

void releaseRounds(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        if (round.mapped != nullptr)
        {
            ::munmap(round.mapped, PlacementSize);
        }
        if (round.handle >= 0)
        {
            ::close(round.handle);
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <mount> <rounds> <tag> [phase]\n", argv[0]);
        return 2;
    }

    const Mount mount{argv[1]};
    const auto wanted = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (wanted < LeastRounds)
    {
        std::fprintf(stderr, "rounds must be at least %u\n", LeastRounds);
        return 2;
    }

    const std::string_view tag{argv[3]};
    const std::string_view only{(argc > 4) ? argv[4] : "all"};

    // A mount with no node_id is a baseline rather than a refusal, and its turn column reads 0.
    const auto found = findNodeOfMount(mount.getPoint());
    const auto node = static_cast<std::uint32_t>((found < 0) ? 0 : found);

    std::vector<Round_t> rounds;
    rounds.reserve(wanted);
    for (std::uint32_t idx = 0; idx < wanted; ++idx)
    {
        rounds.push_back(Round_t{mount.pathTo("probe-" + std::string{tag} + '-' + std::to_string(idx))});
    }

    const char* gapText = std::getenv("PROBE_GAP_US");
    const auto gapUs =
        static_cast<std::uint32_t>((gapText != nullptr) ? std::strtoul(gapText, nullptr, 10) : 0);

    const Run_t run{tag, only, node, gapUs};
    if (!takeNamePhases(run, rounds) || !takeMapPhases(run, rounds))
    {
        releaseRounds(rounds);
        return 1;
    }

    // The two below want the descriptors gone: a lookup that found an open one would price a
    // second open on this process's own file rather than the path a caller takes.
    releaseRounds(rounds);

    const auto readOnly = [](auto&& run)
    {
        return [run](std::vector<Round_t>& each)
        {
            run(each);
            return true;
        };
    };
    if (!takePhase("lookup", run, rounds, readOnly(runLookup)))
    {
        return 1;
    }

    if (!takePhase("unlink", run, rounds, [&run](std::vector<Round_t>& each)
                   {
                       runUnlink(each, run.gapUs);
                       return true;
                   }))
    {
        return 1;
    }

    return 0;
}
