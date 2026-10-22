// SPDX-License-Identifier: Apache-2.0
//
// test_second_device_free -- a paged region on the second device takes space from its pool and an unlink
// gives it all back.
//
// Reads the second device's free bytes off pool_info, so nothing else may be placing meanwhile. A
// mount of one device has no second device, so the case skips.
//
//   test_second_device_free <mount-a>

#include <cstdint>
#include <optional>
#include <string>

#include "common/poll.hpp"
#include "common/timing.hpp"
#include "fs/file.hpp"
#include "harness/device.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

constexpr timing::Secs FreeDeadline{10};
constexpr timing::Millis FreePoll{100};

[[nodiscard]] std::optional<std::uint64_t> secondDeviceFree()
{
    const auto pools = fsuser::tests::secondDevicePool();
    if (!pools)
    {
        return std::nullopt;
    }
    return pools->devices.at(fsuser::tests::SecondDevice).wbFree;
}

void checkFreeReturns(fsuser::tests::Report& report, fsuser::tests::Mount& mount, const std::string& name,
                      std::uint64_t before)
{
    {
        auto file = fsuser::tests::placeOnDevice(mount, name, fsuser::tests::SecondDeviceAttr, PlacementSize,
                                                 fsuser::Placement::Paged);
        report.check("the paged region lies on device 1", fsuser::tests::allPiecesOn(name, fsuser::tests::SecondDevice));
        const auto held = secondDeviceFree();
        report.check("device 1 has less free space while the region stands", held && *held < before);
    }

    mount.unlink(name);
    report.check("device 1's free space returns to what it was",
                 polling::waitUntil(
                     [before]
                     {
                         return secondDeviceFree() == before;
                     },
                     FreeDeadline, FreePoll));
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        const auto before = secondDeviceFree();
        if (!before)
        {
            report.note(fsuser::tests::NoSecondDeviceNote);
            return;
        }

        const auto name = caseName("device2_free");
        auto mount = fsuser::tests::Mount(mounts.first());
        checkFreeReturns(report, mount, name, *before);
    };
    return fsuser::tests::runCase(argc, argv, "test_second_device_free", 1, body);
}
