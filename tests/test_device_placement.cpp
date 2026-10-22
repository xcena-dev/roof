// SPDX-License-Identifier: Apache-2.0
//
// test_device_placement -- a region asked onto a device lands on it, and the asks the mount cannot
// serve are refused before any region exists.
//
// RegionAttr_t::device is 1 plus the device index, so 1 is the primary. extents/<slot> shows
// the device a piece landed on.
//
//   test_device_placement <mount-a>

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
using fsuser::tests::makeDeviceAttr;
using fsuser::tests::PlacementSize;
using fsuser::tests::PrimaryDevice;
using fsuser::tests::PrimaryDeviceAttr;
using fsuser::tests::SecondDevice;
using fsuser::tests::SecondDeviceAttr;

void checkPrimaryPlacement(fsuser::tests::Report& report, fsuser::tests::Mount& mount, const std::string& name)
{
    report.section("a file asked for the primary device lands on device 0");

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
    report.checkAccepted("the device is accepted before placement", [&]
                         {
                             file.setRegionAttr(makeDeviceAttr(PrimaryDeviceAttr));
                         });
    report.check("reading it back gives what was asked", file.readRegionAttr().device == PrimaryDeviceAttr);

    file.resize(PlacementSize);

    report.check("the placed region reads back as the primary device", file.readRegionAttr().device == PrimaryDeviceAttr);
    const auto pieces = fsuser::tests::readPiecesOf(name);
    report.check("extents shows one piece on device 0", pieces.size() == 1 && pieces.front().device == PrimaryDevice);

    report.checkRefusedWith("changing it after placement answers EBUSY", std::errc::device_or_resource_busy,
                            [&]
                            {
                                file.setRegionAttr(makeDeviceAttr(fsuser::AnyDevice));
                            });
}

void checkRefusals(fsuser::tests::Report& report, fsuser::tests::Mount& mount, const std::string& name,
                   std::uint32_t deviceCount)
{
    report.section("what the mount refuses");

    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
    report.checkRefusedWith("a device past the mount's count answers EINVAL", std::errc::invalid_argument,
                            [&]
                            {
                                file.setRegionAttr(makeDeviceAttr(deviceCount + 1));
                            });
    report.check("the refused ask leaves the wish unset", file.readRegionAttr().device == fsuser::AnyDevice);

    if (deviceCount <= SecondDevice)
    {
        report.section("the mount has one device, so uncached on a second one has nothing to refuse");
        return;
    }
    report.checkRefusedWith("uncached on a device other than the primary answers EINVAL",
                            std::errc::invalid_argument,
                            [&]
                            {
                                file.setRegionAttr(makeDeviceAttr(SecondDeviceAttr, fsuser::CachePolicy::Uncached));
                            });
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto pools = fsuser::tests::readPoolInfo();
        if (!pools || pools->devices.empty())
        {
            report.check("pool_info lists the devices", false);
            return;
        }

        const auto placedName = caseName("device_placed");
        const auto refusedName = caseName("device_bad");
        auto mount = fsuser::tests::Mount(mounts.first());

        checkPrimaryPlacement(report, mount, placedName);
        mount.unlink(placedName);

        checkRefusals(report, mount, refusedName, static_cast<std::uint32_t>(pools->devices.size()));
        mount.unlink(refusedName);
    };
    return fsuser::tests::runCase(argc, argv, "test_device_placement", 1, body);
}
