// SPDX-License-Identifier: Apache-2.0
//
// test_cache_policy -- a file placed in the pool the caller asked for, and mapped the way that pool
// is mapped.
//
// The policy is not stored on the file: it is where the region sits. So the check has two halves
// that have to agree. region_info says which side of the boundary the extent landed on, and the
// vma's own flags say which mmap path the kernel then took. VM_PFNMAP is the uncached one, which
// carries no struct pages, and VM_MIXEDMAP is the write-back one, which does.
//
//   test_cache_policy <mount-a>

#include <fcntl.h>
#include <sys/mman.h>

#include <cstdint>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;
using fsuser::tests::PoolInfo_t;

// The abbreviations /proc/<pid>/smaps prints for the two flags that tell the mapping paths apart.
constexpr const char* PfnMapFlag = "pf";
constexpr const char* MixedMapFlag = "mm";

// The VmFlags line of the mapping that covers @address, or an empty string when no region does.
// smaps names each region by its address span, and VmFlags is the last line of that region's block.
[[nodiscard]] std::string readVmFlags(const void* address)
{
    const auto wanted = reinterpret_cast<std::uintptr_t>(address);

    std::ifstream source{"/proc/self/smaps"};
    std::string line;
    bool inside = false;

    while (std::getline(source, line))
    {
        const auto dash = line.find('-');
        const auto space = line.find(' ');
        // A header line opens a region: "<start>-<end> perms ...". Anything else belongs to it.
        if (dash != std::string::npos && space != std::string::npos && dash < space)
        {
            const auto start = std::stoull(line.substr(0, dash), nullptr, 16);
            const auto end = std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16);
            inside = wanted >= start && wanted < end;
            continue;
        }

        if (inside && line.rfind("VmFlags:", 0) == 0)
        {
            return line;
        }
    }
    return {};
}

[[nodiscard]] bool flagsHold(const std::string& vmFlags, const char* wanted)
{
    return vmFlags.find(std::string{" "} + wanted) != std::string::npos;
}

// The extent region_info reports for @name, or 0 when no row carries it.
[[nodiscard]] std::uint64_t readPlacedOffset(const std::string& name)
{
    for (const auto& row : fsuser::tests::readRegionRows())
    {
        if (row.name == name)
        {
            return row.offset;
        }
    }
    return 0;
}

void checkOnePolicy(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                    const PoolInfo_t& pools, const std::string& name, fsuser::CachePolicy policy)
{
    const bool wantUncached = policy == fsuser::CachePolicy::Uncached;
    report.section(wantUncached ? "a file asked for the uncached pool"
                                : "a file asked for the write-back pool");

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);

    report.checkAccepted("the policy is accepted before placement", [&]
                         {
                             file.setCachePolicy(policy);
                         });
    report.check("reading it back gives what was asked", file.readCachePolicy() == policy);

    file.resize(PlacementSize);

    const auto placed = readPlacedOffset(name);
    report.check("the extent is placed at all", placed != 0);
    report.check(wantUncached ? "the extent sits below the boundary"
                              : "the extent sits above the boundary",
                 wantUncached ? (placed >= pools.ucStart && placed < pools.wbStart)
                              : (placed >= pools.wbStart));

    report.check("reading it back after placement still agrees", file.readCachePolicy() == policy);

    auto mapping = file.map(PlacementSize, PROT_READ);
    const auto vmFlags = readVmFlags(mapping.get());
    report.check("the mapping is found in smaps", !vmFlags.empty());
    report.check(wantUncached ? "the mapping is VM_PFNMAP" : "the mapping is VM_MIXEDMAP",
                 flagsHold(vmFlags, wantUncached ? PfnMapFlag : MixedMapFlag));
    report.check(wantUncached ? "the mapping is not VM_MIXEDMAP" : "the mapping is not VM_PFNMAP",
                 !flagsHold(vmFlags, wantUncached ? MixedMapFlag : PfnMapFlag));

    // A placed region has its pool fixed, since another pool means other bytes.
    report.checkRefusedWith("changing it after placement answers EBUSY", std::errc::device_or_resource_busy,
                            [&]
                            {
                                file.setCachePolicy(wantUncached ? fsuser::CachePolicy::Writeback
                                                                 : fsuser::CachePolicy::Uncached);
                            });
}

void checkRefusals(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                   const std::string& name)
{
    report.section("what the policy refuses");

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);

    report.checkRefusedWith("a policy outside the enum answers EINVAL", std::errc::invalid_argument,
                            [&]
                            {
                                // A value outside the enum is the point: the kernel has to refuse it.
                                // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                                file.setCachePolicy(static_cast<fsuser::CachePolicy>(7));
                            });
    report.check("the default is write-back", file.readCachePolicy() == fsuser::CachePolicy::Writeback);
}

}  // namespace

int main(int argc, char** argv)
{
    fsuser::tests::Mounts_t mounts;
    if (!fsuser::tests::takeMounts(argc, argv, 1, mounts))
    {
        return 2;
    }

    fsuser::tests::Report report;
    const auto uncachedName = caseName("cache_uc");
    const auto writebackName = caseName("cache_wb");
    const auto refusedName = caseName("cache_bad");

    try
    {
        const auto pools = fsuser::tests::readPoolInfo();
        if (!pools)
        {
            report.check("pool_info is readable", false);
            return report.summarise("test_cache_policy");
        }

        auto mount = fsuser::tests::Mount(mounts.first());

        checkOnePolicy(report, mount, *pools, writebackName, fsuser::CachePolicy::Writeback);
        mount.unlink(writebackName);

        // The uncached pool is the smaller one and may be full, which is a result and not a fault.
        if (pools->ucFree >= PlacementSize)
        {
            checkOnePolicy(report, mount, *pools, uncachedName, fsuser::CachePolicy::Uncached);
            mount.unlink(uncachedName);
        }
        else
        {
            report.section("the uncached pool has no room for this case");
        }

        checkRefusals(report, mount, refusedName);
        mount.unlink(refusedName);
    }
    catch (const std::exception& failure)
    {
        report.raised("test_cache_policy setup", failure);
    }

    return report.summarise("test_cache_policy");
}
