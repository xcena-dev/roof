// SPDX-License-Identifier: Apache-2.0
//
// probe_perm -- how long one permission ioctl takes on one mount.
//
// The three ioctls a delegation passes through are priced separately, because they do different
// amounts of work under the same turn: a grant fills a row, a default touches only the shared line,
// and a revoke scans the table for the row naming an account.
//
// Each round owns its own name, so a sample waits on no sample before it except through the turn.
// @prefill lays that many rows down on each name before the window opens, which is what separates a
// scan over an empty table from a scan over a full one.
//
// Not a case. It asserts nothing and returns 0 whatever it measures.
//
//   probe_perm <mount> <rounds> <tag> [prefill]

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"
#include "uapi.h"

namespace
{

using fsuser::tests::Mount;
using fsuser::tests::PlacementSize;
using fsuser::tests::readMetaTurns;

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t LeastRounds = 4;

// The table holds this many rows, so a prefill above it would only be refused.
constexpr std::uint32_t DelegRows = 29;

// The account a timed row names. It is nobody on this host, which is what keeps a grant from
// widening a login that exists.
constexpr std::uint32_t TimedUid = 65000;

struct Round_t
{
    std::string name;
    std::int32_t handle{-1};
    double took{0.0};
};

[[nodiscard]] double takeElapsedUs(Clock::time_point began, Clock::time_point ended)
{
    return std::chrono::duration<double, std::micro>{ended - began}.count();
}

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

void printPhase(std::string_view tag, std::string_view phase, const std::vector<Round_t>& rounds,
                std::uint64_t turns)
{
    std::vector<double> samples;
    samples.reserve(rounds.size());
    for (const auto& round : rounds)
    {
        samples.push_back(round.took);
    }
    std::sort(samples.begin(), samples.end());

    const auto count = samples.size();
    if (count == 0)
    {
        std::printf("%s,%s,0,,,,,,0\n", std::string{tag}.c_str(), std::string{phase}.c_str());
        std::fflush(stdout);
        return;
    }
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::printf("%s,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%llu\n", std::string{tag}.c_str(),
                std::string{phase}.c_str(), static_cast<unsigned long long>(count),
                samples.front(), samples[count / 2], samples[(count * 95) / 100], samples.back(),
                total / static_cast<double>(count), static_cast<unsigned long long>(turns));
    std::fflush(stdout);
}

[[nodiscard]] fs_perm_req makeRequest(std::uint32_t uid, std::uint32_t perms)
{
    fs_perm_req request{};
    request.uid = uid;
    request.gid = FS_PERM_ANY_ID;
    request.perms = perms;
    return request;
}

// The rows a timed call has to look past. They name accounts the timed one never does, so the scan
// reaches its own row only after walking every one of these.
[[nodiscard]] bool layPrefill(std::int32_t handle, std::uint32_t rows)
{
    for (std::uint32_t row = 0; row < rows; ++row)
    {
        auto request = makeRequest(TimedUid + 1 + row, FS_PERM_READ);
        if (::ioctl(handle, FS_IOC_PERM_GRANT, &request) != 0)
        {
            std::perror("ioctl PERM_GRANT prefill");
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool runGrant(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        auto request = makeRequest(TimedUid, FS_PERM_READ);
        const auto began = Clock::now();
        const auto failed = ::ioctl(round.handle, FS_IOC_PERM_GRANT, &request);
        round.took = takeElapsedUs(began, Clock::now());
        if (failed != 0)
        {
            std::perror("ioctl PERM_GRANT");
            return false;
        }
    }
    return true;
}

// The default touches the shared line alone, so this is the same turn without the table walk.
[[nodiscard]] bool runDefault(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        auto request = makeRequest(FS_PERM_ANY_ID, FS_PERM_READ);
        const auto began = Clock::now();
        const auto failed = ::ioctl(round.handle, FS_IOC_PERM_SET_DEFAULT, &request);
        round.took = takeElapsedUs(began, Clock::now());
        if (failed != 0)
        {
            std::perror("ioctl PERM_SET_DEFAULT");
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool runRevoke(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        auto request = makeRequest(TimedUid, 0);
        const auto began = Clock::now();
        const auto failed = ::ioctl(round.handle, FS_IOC_PERM_REVOKE, &request);
        round.took = takeElapsedUs(began, Clock::now());
        if (failed != 0)
        {
            std::perror("ioctl PERM_REVOKE");
            return false;
        }
    }
    return true;
}

template <typename T_Phase>
[[nodiscard]] bool takePhase(std::string_view name, std::string_view tag, std::uint32_t node,
                             std::vector<Round_t>& rounds, T_Phase&& phase)
{
    const auto before = readTurnsTaken(node);
    if (!phase(rounds))
    {
        return false;
    }
    printPhase(tag, name, rounds, readTurnsTaken(node) - before);
    return true;
}

// O_EXCL means a valid handle is this run's own creation, so only that name is this run's to
// remove. A round that never opened, or lost O_EXCL to an existing file, names something else.
void releaseRounds(std::vector<Round_t>& rounds)
{
    for (auto& round : rounds)
    {
        if (round.handle >= 0)
        {
            ::close(round.handle);
            ::unlink(round.name.c_str());
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <mount> <rounds> <tag> [prefill]\n", argv[0]);
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
    auto prefill = (argc > 4) ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 0U;
    if (prefill >= DelegRows)
    {
        prefill = DelegRows - 1;
    }

    const auto found = findNodeOfMount(mount.getPoint());
    const auto node = static_cast<std::uint32_t>((found < 0) ? 0 : found);

    std::vector<Round_t> rounds;
    rounds.reserve(wanted);
    for (std::uint32_t idx = 0; idx < wanted; ++idx)
    {
        rounds.push_back(Round_t{mount.pathTo("perm-" + std::string{tag} + '-' + std::to_string(idx))});
    }

    for (auto& round : rounds)
    {
        round.handle = ::open(round.name.c_str(), O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0644);
        if (round.handle < 0)
        {
            std::perror("open O_CREAT");
            releaseRounds(rounds);
            return 1;
        }
        if (::ftruncate(round.handle, static_cast<::off_t>(PlacementSize)) != 0)
        {
            std::perror("ftruncate");
            releaseRounds(rounds);
            return 1;
        }
        if (!layPrefill(round.handle, prefill))
        {
            releaseRounds(rounds);
            return 1;
        }
    }

    const auto ran = takePhase("grant", tag, node, rounds, runGrant) &&
                     takePhase("setdefault", tag, node, rounds, runDefault) &&
                     takePhase("revoke", tag, node, rounds, runRevoke);

    releaseRounds(rounds);
    return ran ? 0 : 1;
}
