// SPDX-License-Identifier: Apache-2.0
//
// test_second_device_placement -- a region asked onto the second device lies on device 1, a region too
// large for device 0 goes there by itself, and uncached cannot be put there.
//
// A mount of one device has no second device, so the case skips.
//
//   test_second_device_placement <mount-a>

#include <fcntl.h>

#include <cstdint>
#include <string>
#include <system_error>

#include "fs/file.hpp"
#include "harness/device.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

void checkAskedOntoSecond(fsuser::tests::Report& report, fsuser::tests::Mount& mount, const std::string& name)
{
    report.section("a region asked onto the second device has every piece on device 1");

    auto file = fsuser::tests::placeOnDevice(mount, name, fsuser::tests::SecondDeviceAttr, PlacementSize);
    report.check("the placed region reads back as the second device",
                 file.readRegionAttr().device == fsuser::tests::SecondDeviceAttr);
    report.check("every piece lies on device 1", fsuser::tests::allPiecesOn(name, fsuser::tests::SecondDevice));
}

// False when device 1 has no room to test with, so no name was made.
[[nodiscard]] bool checkTooLargeForPrimary(fsuser::tests::Report& report, fsuser::tests::Mount& mount,
                                           const std::string& name, const fsuser::tests::PoolInfo_t& pools)
{
    const auto& primary = pools.devices.at(fsuser::tests::PrimaryDevice);
    const auto& second = pools.devices.at(fsuser::tests::SecondDevice);
    const std::uint64_t bytes = primary.wbFree + pools.granule;

    report.section("a contiguous region larger than device 0's free space lands on device 1");
    if (pools.granule == 0 || bytes > second.wbFree)
    {
        report.note("device 1 has no room beyond device 0's free space, so this check is skipped");
        return false;
    }

    auto file = fsuser::tests::placeOnDevice(mount, name, fsuser::AnyDevice, bytes);
    report.check("every piece lies on device 1", fsuser::tests::allPiecesOn(name, fsuser::tests::SecondDevice));
    return true;
}

void checkUncachedRefused(fsuser::tests::Report& report, fsuser::tests::Mount& mount, const std::string& name)
{
    report.section("uncached on the second device");

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
    report.checkRefusedWith("uncached on the second device answers EINVAL", std::errc::invalid_argument,
                            [&]
                            {
                                file.setRegionAttr(fsuser::tests::makeDeviceAttr(fsuser::tests::SecondDeviceAttr,
                                                                                 fsuser::CachePolicy::Uncached));
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto pools = fsuser::tests::secondDevicePool();
        if (!pools)
        {
            report.note(fsuser::tests::NoSecondDeviceNote);
            return;
        }

        const auto askedName = caseName("device2_asked");
        const auto largeName = caseName("device2_large");
        const auto uncachedName = caseName("device2_uc");

        auto mount = fsuser::tests::Mount(mounts.first());
        checkAskedOntoSecond(report, mount, askedName);
        mount.unlink(askedName);

        if (checkTooLargeForPrimary(report, mount, largeName, *pools))
        {
            mount.unlink(largeName);
        }

        checkUncachedRefused(report, mount, uncachedName);
        mount.unlink(uncachedName);
    };
    return fsuser::tests::runCase(argc, argv, "test_second_device_placement", 1, body);
}
