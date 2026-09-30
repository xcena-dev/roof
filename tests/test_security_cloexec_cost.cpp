// SPDX-License-Identifier: Apache-2.0
//
// test_security_cloexec_cost -- what the close-on-exec guard costs a caller with many descriptors.
//
// Every read, mapping and ioctl checks that the descriptor it was handed is close-on-exec, and the
// only way to reach that bit from a struct file is to find the descriptor again. A walk of the table
// from index 0 would cost what the caller chooses by how many descriptors it opened first, so the
// claim is that a read costs the same wherever the descriptor sits.
//
// Raising the descriptor limit alone changes nothing, because the table grows with the descriptors
// actually opened. The comparison therefore opens them.
//
//   test_security_cloexec_cost <mount-a>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <ratio>
#include <string>
#include <vector>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

// How many reads each half of the comparison makes. Enough that scheduler noise averages out and
// short enough that the case stays well under a second.
constexpr std::uint32_t ReadRounds = 2000;

// How far down the table the second open is pushed. Any caller may reach this without privilege.
constexpr std::uint32_t PaddingDescriptors = 60000;

// Above this the guard is reading the caller's table rather than the file. Chosen well clear of
// host noise, so a green line means the walk is bounded and not that the machine was quiet.
constexpr double AllowedRatio = 4.0;

// The wall time ReadRounds one-byte reads take on @descriptor.
[[nodiscard]] double measureReadMicros(std::int32_t descriptor)
{
    char byte = 0;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint32_t round = 0; round < ReadRounds; ++round)
    {
        const auto got = ::pread(descriptor, &byte, 1, 0);
        static_cast<void>(got);
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;
    return std::chrono::duration<double, std::micro>{elapsed}.count() / ReadRounds;
}

// Raises the soft descriptor limit as far as the hard limit allows, so the padding below has room.
// Answers what it reached, and 0 when the limit could not be read at all.
[[nodiscard]] ::rlim_t widenDescriptorLimit()
{
    struct ::rlimit limit
    {
    };
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
    {
        return 0;
    }
    limit.rlim_cur = limit.rlim_max;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
    {
        return 0;
    }
    return limit.rlim_cur;
}

// Descriptors held only to push the next open further down the table, closed together at the end.
class Padding
{
public:
    // Opens up to @wanted descriptors on a file that costs the kernel nothing to hold.
    explicit Padding(std::uint32_t wanted)
    {
        held_.reserve(wanted);
        for (std::uint32_t taken = 0; taken < wanted; ++taken)
        {
            const std::int32_t opened = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (opened < 0)
            {
                return;
            }
            held_.push_back(opened);
        }
    }

    Padding(const Padding&) = delete;
    Padding& operator=(const Padding&) = delete;
    Padding(Padding&&) = delete;
    Padding& operator=(Padding&&) = delete;

    ~Padding()
    {
        for (const std::int32_t opened : held_)
        {
            ::close(opened);
        }
    }

    [[nodiscard]] std::uint64_t count() const noexcept
    {
        return held_.size();
    }

private:
    std::vector<std::int32_t> held_;
};

void checkGuardCostIsBounded(fsuser::tests::Report& report, fsuser::tests::Mount& mount)
{
    report.section("what the close-on-exec guard costs a caller holding many descriptors");

    const auto name = caseName("security_cloexec_cost");

    try
    {
        auto file = fsuser::tests::placeFile(mount, name);
        {
            // A byte has to be there for the read to reach the guard rather than the size check.
            auto mapped = file.map(PlacementSize, PROT_READ | PROT_WRITE);
            report.check("the region maps for the first write", mapped.isMapped());
        }

        const double nearMicros = measureReadMicros(file.get());
        report.note("read on a descriptor at index " + std::to_string(file.get()) + ": " +
                    std::to_string(nearMicros) + "us");

        const auto reached = widenDescriptorLimit();
        report.check("the descriptor limit widens", reached > PaddingDescriptors);

        const Padding padding{PaddingDescriptors};
        report.note("padding descriptors opened: " + std::to_string(padding.count()));
        if (padding.count() < PaddingDescriptors)
        {
            report.note("this host would not open enough descriptors to drive the comparison");
            mount.unlink(name);
            return;
        }

        // A second open of the same name, which lands past the padding rather than beside it.
        auto far = mount.open(name, O_RDWR | O_CLOEXEC);
        const double farMicros = measureReadMicros(far.get());
        report.note("read on a descriptor at index " + std::to_string(far.get()) + ": " +
                    std::to_string(farMicros) + "us");

        const double ratio = nearMicros > 0.0 ? farMicros / nearMicros : 0.0;
        report.note("ratio: " + std::to_string(ratio));
        report.check(
            "a read costs the same wherever the descriptor sits in the table "
            "(open: it walks to it)",
            ratio < AllowedRatio);
    }
    catch (const std::exception& failure)
    {
        report.raised("security cloexec cost", failure);
    }

    mount.unlink(name);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    // Before either reading is taken, so the near one and the far one are timed under one account.
    static_cast<void>(fsuser::tests::dropRealIdsToInvoker());

    fsuser::tests::Report report;
    try
    {
        auto mount = fsuser::tests::Mount(mounts.first());
        checkGuardCostIsBounded(report, mount);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_security_cloexec_cost", failure);
    }

    return report.summarise("test_security_cloexec_cost");
}
