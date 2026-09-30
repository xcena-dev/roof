// SPDX-License-Identifier: Apache-2.0
//
// probe_data -- what a data access costs once the name and the extent are already there.
//
// Three calls sit on this path and none of them takes a turn. read() walks the permission rows and
// copies; a store into a standing mapping reaches CXL with nothing of this module in the way; and a
// second mmap of a name already delegated skips the upcall the first one paid.
//
// read() is timed at two sizes by the caller running it twice, which is what separates the fixed
// cost of the permission walk from the copy that rides on it.
//
// The line pass reports one sweep of the region at cacheline stride rather than one access, because
// a single 8-byte store lands under the clock's own call overhead and would read as that instead.
//
// Not a case. It asserts nothing and returns 0 whatever it measures.
//
// @padfds opens that many descriptors on another device before the window. Nothing in the timed
// calls touches them, so a cost that moves with the count is a cost that walks the table rather than
// the file.
//
//   probe_data <mount> <rounds> <tag> [bytes] [padfds]

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::Mount;
using fsuser::tests::PlacementSize;

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t LeastRounds = 4;
constexpr std::uint64_t LineBytes = 64;
constexpr std::uint64_t DefaultRead = 4096;

[[nodiscard]] double takeElapsedUs(Clock::time_point began, Clock::time_point ended)
{
    return std::chrono::duration<double, std::micro>{ended - began}.count();
}

void printPhase(std::string_view tag, std::string_view phase, std::vector<double>& samples)
{
    std::sort(samples.begin(), samples.end());
    const auto count = samples.size();
    if (count == 0)
    {
        std::printf("%s,%s,0,,,,,\n", std::string{tag}.c_str(), std::string{phase}.c_str());
        std::fflush(stdout);
        return;
    }
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::printf("%s,%s,%llu,%.2f,%.2f,%.2f,%.2f,%.2f\n", std::string{tag}.c_str(),
                std::string{phase}.c_str(), static_cast<unsigned long long>(count),
                samples.front(), samples[count / 2], samples[(count * 95) / 100], samples.back(),
                total / static_cast<double>(count));
    std::fflush(stdout);
}

// read(): the permission walk and the copy, on a descriptor already open, so nothing here pays for
// a lookup. pread keeps the offset out of the window.
[[nodiscard]] bool runRead(std::int32_t handle, std::uint64_t bytes, std::uint32_t rounds,
                           std::vector<double>& samples)
{
    std::vector<char> sink(bytes);
    for (std::uint32_t round = 0; round < rounds; ++round)
    {
        const auto began = Clock::now();
        const auto got = ::pread(handle, sink.data(), bytes, 0);
        const auto took = takeElapsedUs(began, Clock::now());
        if (got < 0)
        {
            std::perror("pread");
            return false;
        }
        samples.push_back(took);
    }
    return true;
}

// One store per cacheline over the whole region, on pages already faulted in. What this prices is
// the memory and not this module: no call of ours stands between the store and CXL.
void runLinePass(void* mapped, std::uint32_t rounds, std::vector<double>& samples)
{
    auto* bytes = static_cast<volatile std::uint64_t*>(mapped);
    const auto stride = LineBytes / sizeof(std::uint64_t);
    const auto words = PlacementSize / sizeof(std::uint64_t);

    for (std::uint32_t round = 0; round < rounds; ++round)
    {
        const auto began = Clock::now();
        for (std::uint64_t index = 0; index < words; index += stride)
        {
            bytes[index] = index;
        }
        samples.push_back(takeElapsedUs(began, Clock::now()));
    }
}

// The second mapping of a name this caller already holds a row for. The first one earned the row
// through an upcall, so what is left here is the mmap itself.
void runRemap(std::int32_t handle, std::uint32_t rounds, std::vector<double>& samples)
{
    for (std::uint32_t round = 0; round < rounds; ++round)
    {
        const auto began = Clock::now();
        void* mapped = ::mmap(nullptr, PlacementSize, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
        const auto took = takeElapsedUs(began, Clock::now());
        if (mapped == MAP_FAILED)
        {
            std::perror("mmap");
            return;
        }
        samples.push_back(took);
        ::munmap(mapped, PlacementSize);
    }
}

// Descriptors this process holds and never uses. They widen the caller's table, which is the only
// thing they are here to do.
[[nodiscard]] std::vector<std::int32_t> openPadding(std::uint32_t howMany)
{
    std::vector<std::int32_t> held;
    held.reserve(howMany);
    for (std::uint32_t each = 0; each < howMany; ++each)
    {
        const auto handle = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (handle < 0)
        {
            break;
        }
        held.push_back(handle);
    }
    return held;
}

void closePadding(const std::vector<std::int32_t>& held)
{
    for (const auto handle : held)
    {
        ::close(handle);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <mount> <rounds> <tag> [bytes] [padfds]\n", argv[0]);
        return 2;
    }

    const Mount mount{argv[1]};
    const auto rounds = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (rounds < LeastRounds)
    {
        std::fprintf(stderr, "rounds must be at least %u\n", LeastRounds);
        return 2;
    }

    const std::string_view tag{argv[3]};
    auto bytes = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : DefaultRead;
    if (bytes == 0 || bytes > PlacementSize)
    {
        bytes = DefaultRead;
    }

    const auto padding =
        openPadding((argc > 5) ? static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10)) : 0U);

    const auto name = mount.pathTo("data-" + std::string{tag});
    const auto handle = ::open(name.c_str(), O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0644);
    if (handle < 0)
    {
        std::perror("open O_CREAT");
        return 1;
    }

    auto done = 1;
    if (::ftruncate(handle, static_cast<::off_t>(PlacementSize)) == 0)
    {
        void* mapped = ::mmap(nullptr, PlacementSize, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
        if (mapped == MAP_FAILED)
        {
            std::perror("mmap");
        }
        else
        {
            // Every page in before the window opens, so the pass below prices stores and not faults.
            std::memset(mapped, 0, PlacementSize);

            std::vector<double> reads;
            std::vector<double> lines;
            std::vector<double> remaps;
            if (runRead(handle, bytes, rounds, reads))
            {
                printPhase(tag, "read" + std::to_string(bytes), reads);
                runLinePass(mapped, rounds, lines);
                printPhase(tag, "linepass", lines);
                runRemap(handle, rounds, remaps);
                printPhase(tag, "remap", remaps);
                done = 0;
            }
            ::munmap(mapped, PlacementSize);
        }
    }
    else
    {
        std::perror("ftruncate");
    }

    ::close(handle);
    ::unlink(name.c_str());
    closePadding(padding);
    return done;
}
