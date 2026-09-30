// SPDX-License-Identifier: Apache-2.0
//
// probe_access -- what one access upcall costs, measured from a consumer that is not the owner.
//
// The access upcall fires only when a caller with no delegation maps a region: the owner passes the
// local check and never reaches the helper. So this needs two nodes. The owner mount creates and
// places the regions and leaves the default at owner-only, and the consumer mount maps each one for
// the first time. That first map fails the local check, so the kernel asks the helper, which attests
// the consumer and runs the policy before writing the delegation that lets the map succeed.
//
// One region per round, because the delegation the first map earns would let a second map skip the
// upcall. So a phase of N rounds is N distinct upcalls, each measured once.
//
//   probe_access <owner-mount> <consumer-mount> <rounds> <tag>
//
// Not a case. It asserts nothing and returns 0 whatever it measures.

#include <fcntl.h>
#include <sys/mman.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/stats.hpp"

namespace
{

using fsuser::tests::PlacementSize;

// A microsecond delta between two steady-clock reads.
[[nodiscard]] double sinceUs(std::chrono::steady_clock::time_point began)
{
    const auto delta = std::chrono::steady_clock::now() - began;
    return std::chrono::duration<double, std::micro>(delta).count();
}

// One CSV row: tag,phase,count,min,p50,p95,max,mean. Same shape probe_ops prints, minus the turn
// column, since what this prices is the upcall and not a turn.
void printPhase(std::string_view tag, std::string_view phase, std::vector<double>& samples)
{
    const auto summary = fsuser::tests::summarise(samples);
    if (!summary)
    {
        std::printf("%s,%s,0,,,,,\n", std::string{tag}.c_str(), std::string{phase}.c_str());
        return;
    }
    std::printf("%s,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f\n", std::string{tag}.c_str(),
                std::string{phase}.c_str(), static_cast<unsigned long long>(summary->count),
                summary->least, summary->median, summary->p95, summary->most, summary->mean);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <owner-mount> <consumer-mount> <rounds> <tag>\n", argv[0]);
        return 2;
    }

    const fsuser::tests::Mount owner{argv[1]};
    const fsuser::tests::Mount consumer{argv[2]};
    const auto rounds = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const std::string tag{argv[4]};
    if (rounds < 4)
    {
        std::fprintf(stderr, "rounds must be at least 4\n");
        return 2;
    }

    std::vector<std::string> names;
    names.reserve(rounds);
    for (std::uint32_t index = 0; index < rounds; ++index)
    {
        names.push_back("access-" + tag + '-' + std::to_string(index));
    }

    // Owner side: create and place every region, default left at owner-only so the consumer's map
    // has no local right and must reach the helper.
    try
    {
        for (const auto& name : names)
        {
            auto file = owner.open(name, O_CREAT | O_RDWR, 0644);
            file.resize(PlacementSize);
        }
    }
    catch (const std::exception& setup)
    {
        std::fprintf(stderr, "owner setup failed: %s\n", setup.what());
        return 1;
    }

    // Consumer side: the first map of each region is one access upcall. Time only that map.
    std::vector<double> samples;
    samples.reserve(rounds);
    for (const auto& name : names)
    {
        try
        {
            auto file = consumer.open(name, O_RDWR, 0);
            const auto began = std::chrono::steady_clock::now();
            auto mapping = file.map(PlacementSize, PROT_READ);
            samples.push_back(sinceUs(began));
        }
        catch (const std::exception& denied)
        {
            std::fprintf(stderr, "consumer map of %s failed: %s\n", name.c_str(), denied.what());
        }
    }

    printPhase(tag, "access", samples);

    for (const auto& name : names)
    {
        try
        {
            owner.unlink(name);
        }
        catch (const std::exception& cleanup)
        {
            std::fprintf(stderr, "cleanup unlink %s: %s\n", name.c_str(), cleanup.what());
        }
    }
    return 0;
}
