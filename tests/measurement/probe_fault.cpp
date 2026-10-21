// SPDX-License-Identifier: Apache-2.0
//
// probe_fault -- what the first touch of each 4 KiB page of a fresh mapping costs, against a
// second touch of the same page. A chardev target (device_dax) maps its first <mib> MiB, and write
// mode overwrites them. A directory target gets a file that the run makes and removes.
//
//   probe_fault <mount|chardev> <mib> <tag> <read|write>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ratio>
#include <string>
#include <string_view>

#include "probe_common.hpp"

namespace
{

using fsuser::probes::MiBBytes;
using fsuser::probes::PageBytes;
using fsuser::probes::parseUnsigned;
using fsuser::probes::readMinorFaults;

using Clock = std::chrono::steady_clock;

// A touch slower than the last bucket counts toward the totals only, so a rank there reports the max.
constexpr std::chrono::nanoseconds BucketWidth{10};
constexpr std::int64_t BucketCount = 10000;

enum class Access : std::uint8_t
{
    Read,
    Write,
};

struct Histogram_t
{
    std::array<std::uint64_t, BucketCount> buckets{};
    std::uint64_t count{0};
    std::chrono::nanoseconds total{0};
    std::chrono::nanoseconds fastest{std::chrono::nanoseconds::max()};
    std::chrono::nanoseconds slowest{0};
    std::uint64_t minorFaults{0};

    void add(std::chrono::nanoseconds elapsed)
    {
        const std::int64_t bucket = elapsed / BucketWidth;
        if (bucket < BucketCount)
        {
            ++buckets[static_cast<std::size_t>(bucket)];
        }
        ++count;
        total += elapsed;
        fastest = std::min(fastest, elapsed);
        slowest = std::max(slowest, elapsed);
    }

    // The lower edge of the bucket that holds the sample at @rank, but never below the fastest sample.
    [[nodiscard]] std::chrono::nanoseconds pickRank(double rank) const
    {
        const auto wanted = static_cast<std::uint64_t>(rank * static_cast<double>(count - 1));
        std::uint64_t seen = 0;
        std::chrono::nanoseconds edge{0};
        for (const std::uint64_t inBucket : buckets)
        {
            seen += inBucket;
            if (seen > wanted)
            {
                return std::max(edge, fastest);
            }
            edge += BucketWidth;
        }
        return slowest;
    }
};

// One touch per page in address order. A write stores one byte, which also dirties the page.
[[nodiscard]] Histogram_t touchPages(volatile std::uint8_t* base, std::uint64_t pages, Access access)
{
    Histogram_t histogram{};
    const std::uint64_t faultsBefore = readMinorFaults();
    for (std::uint64_t page = 0; page < pages; ++page)
    {
        volatile std::uint8_t* const cell = base + page * PageBytes;
        const auto began = Clock::now();
        if (access == Access::Write)
        {
            *cell = 1;
        }
        else
        {
            const std::uint8_t seen = *cell;
            (void)seen;
        }
        const auto ended = Clock::now();
        histogram.add(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - began));
    }
    histogram.minorFaults = readMinorFaults() - faultsBefore;
    return histogram;
}

void printPass(std::string_view pass, const Histogram_t& histogram)
{
    using Microseconds = std::chrono::duration<double, std::micro>;
    using Milliseconds = std::chrono::duration<double, std::milli>;
    constexpr double MedianRank = 0.50;
    constexpr double P95Rank = 0.95;
    std::printf("%.*s,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu\n", static_cast<int>(pass.size()), pass.data(),
                static_cast<unsigned long long>(histogram.count), Microseconds{histogram.fastest}.count(),
                Microseconds{histogram.pickRank(MedianRank)}.count(),
                Microseconds{histogram.pickRank(P95Rank)}.count(), Microseconds{histogram.slowest}.count(),
                Microseconds{histogram.total}.count() / static_cast<double>(histogram.count),
                Milliseconds{histogram.total}.count(), static_cast<unsigned long long>(histogram.minorFaults));
}

[[nodiscard]] bool isCharDevice(const std::string& path)
{
    struct stat info = {};
    return ::stat(path.c_str(), &info) == 0 && S_ISCHR(info.st_mode);
}

// Returns the exit status, so main closes and removes what it opened on one path.
[[nodiscard]] std::int32_t mapAndTouch(std::int32_t handle, std::uint64_t bytes, bool sizeFirst, Access access)
{
    if (sizeFirst && ::ftruncate(handle, static_cast<::off_t>(bytes)) != 0)
    {
        std::perror("ftruncate");
        return EXIT_FAILURE;
    }
    auto* const mapped =
        ::mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    if (mapped == MAP_FAILED)
    {
        std::perror("mmap");
        return EXIT_FAILURE;
    }

    // The address shows whether the mapping starts on a huge-page boundary.
    std::printf("map,%p\n", mapped);
    std::printf("pass,pages,min_us,median_us,p95_us,max_us,mean_us,total_ms,minflt\n");
    auto* const base = static_cast<volatile std::uint8_t*>(mapped);
    const std::uint64_t pages = bytes / PageBytes;
    printPass("first", touchPages(base, pages, access));
    printPass("second", touchPages(base, pages, access));

    ::munmap(mapped, static_cast<std::size_t>(bytes));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv)
{
    constexpr std::int32_t UsageStatus = 2;
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <mount|chardev> <mib> <tag> <read|write>\n", argv[0]);
        return UsageStatus;
    }

    const auto mib = parseUnsigned(argv[2]);
    if (!mib || *mib == 0)
    {
        std::fprintf(stderr, "mib must be a positive whole number\n");
        return UsageStatus;
    }

    const std::string_view mode{argv[4]};
    if (mode != "read" && mode != "write")
    {
        std::fprintf(stderr, "mode must be read or write\n");
        return UsageStatus;
    }
    const Access access = mode == "write" ? Access::Write : Access::Read;

    const std::string target{argv[1]};
    const bool devdax = isCharDevice(target);
    const std::string path = devdax ? target : std::string{target}.append("/").append(argv[3]).append(".fault");

    constexpr ::mode_t CreateMode = 0644;
    const std::int32_t handle =
        ::open(path.c_str(), devdax ? O_RDWR | O_CLOEXEC : O_CREAT | O_RDWR | O_CLOEXEC, CreateMode);
    if (handle < 0)
    {
        std::perror("open");
        return EXIT_FAILURE;
    }
    const std::int32_t status = mapAndTouch(handle, *mib * MiBBytes, !devdax, access);
    ::close(handle);
    if (!devdax)
    {
        ::unlink(path.c_str());
    }
    return status;
}
