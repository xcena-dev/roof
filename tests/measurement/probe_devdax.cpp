// SPDX-License-Identifier: Apache-2.0
//
// probe_devdax -- what open and mmap cost on the memory itself, with no filesystem in the way.
//
// The floor the other probe's numbers are read against. A device_dax chardev holds no names, so
// only the mapping half has a counterpart here: there is nothing to create, place or unlink.
//
// It maps near the end of the device and reads one byte. The front of a shared device may be in
// use, and PROT_READ leaves every byte as it was either way.
//
// Not a case. It asserts nothing, and it is built without being registered for the same reason the
// probe beside it is.
//
//   probe_devdax <chardev> <rounds> <tag>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <numeric>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

// Below four a percentile lands on the same sample twice, which reads as a spread that is not there.
constexpr std::uint32_t LeastRounds = 4;

// The PMD granularity a device_dax mapping faults at, so a window is aligned to it.
constexpr std::uint64_t HugeSize = 2ULL * 1024ULL * 1024ULL;

// How far back from the end the window sits. Far enough that a run never reaches the front, and a
// whole number of huge pages so the offset stays aligned.
constexpr std::uint64_t TailBack = 64ULL * HugeSize;

struct Round_t
{
    void* mapped{nullptr};
    double took{0.0};
};

[[nodiscard]] double takeElapsedUs(Clock::time_point began, Clock::time_point ended)
{
    return std::chrono::duration<double, std::micro>{ended - began}.count();
}

// The device's own size, so the window is placed from the end rather than from a number written
// here that a differently sized device would put past it.
[[nodiscard]] std::uint64_t readDeviceSize(std::string_view chardev)
{
    const auto slash = chardev.rfind('/');
    const auto leaf = (slash == std::string_view::npos) ? chardev : chardev.substr(slash + 1);
    const std::string path = "/sys/bus/dax/devices/" + std::string{leaf} + "/size";

    std::ifstream source{path};
    std::string line;
    if (!source || !std::getline(source, line))
    {
        return 0;
    }
    try
    {
        return std::stoull(line);
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

void printPhase(std::string_view tag, std::string_view phase, const std::vector<Round_t>& rounds)
{
    std::vector<double> samples;
    samples.reserve(rounds.size());
    for (const auto& round : rounds)
    {
        samples.push_back(round.took);
    }
    if (samples.empty())
    {
        std::printf("%s,%s,0,,,,,,0\n", std::string{tag}.c_str(), std::string{phase}.c_str());
        return;
    }
    std::sort(samples.begin(), samples.end());

    const auto count = samples.size();
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::printf("%s,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,0\n", std::string{tag}.c_str(),
                std::string{phase}.c_str(), static_cast<unsigned long long>(count),
                samples.front(), samples[count / 2], samples[(count * 95) / 100], samples.back(),
                total / static_cast<double>(count));
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <chardev> <rounds> <tag>\n", argv[0]);
        return 2;
    }

    const std::string_view chardev{argv[1]};
    const auto wanted = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const std::string_view tag{argv[3]};
    if (wanted < LeastRounds)
    {
        std::fprintf(stderr, "rounds must be at least %u\n", LeastRounds);
        return 2;
    }

    const auto size = readDeviceSize(chardev);
    if (size < TailBack + static_cast<std::uint64_t>(wanted) * HugeSize)
    {
        std::fprintf(stderr, "%.*s: too small for %u windows behind its tail\n",
                     static_cast<int>(chardev.size()), chardev.data(), wanted);
        return 2;
    }
    const auto tail = ((size - TailBack) / HugeSize) * HugeSize;

    std::vector<Round_t> rounds(wanted);
    const std::string path{chardev};

    // open: the chardev's own, which is the only counterpart a device with no names has for the
    // call. ftruncate and unlink have none: there is no file to resize and none to take away.
    for (auto& round : rounds)
    {
        const auto began = Clock::now();
        const auto opened = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        round.took = takeElapsedUs(began, Clock::now());
        if (opened < 0)
        {
            std::perror("open");
            return 1;
        }
        ::close(opened);
    }
    printPhase(tag, "open", rounds);

    const auto handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0)
    {
        std::perror("open");
        return 1;
    }

    auto offset = tail;
    for (auto& round : rounds)
    {
        const auto began = Clock::now();
        round.mapped = ::mmap(nullptr, HugeSize, PROT_READ, MAP_SHARED, handle,
                              static_cast<::off_t>(offset));
        round.took = takeElapsedUs(began, Clock::now());
        if (round.mapped == MAP_FAILED)
        {
            round.mapped = nullptr;
            std::perror("mmap");
            ::close(handle);
            return 1;
        }
        offset += HugeSize;
    }
    printPhase(tag, "mmap", rounds);

    for (auto& round : rounds)
    {
        const auto* first = static_cast<const volatile unsigned char*>(round.mapped);
        const auto began = Clock::now();
        const auto seen = *first;
        round.took = takeElapsedUs(began, Clock::now());
        (void)seen;
    }
    printPhase(tag, "fault", rounds);

    for (const auto& round : rounds)
    {
        if (round.mapped != nullptr)
        {
            ::munmap(round.mapped, HugeSize);
        }
    }
    ::close(handle);
    return 0;
}
