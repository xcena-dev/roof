// SPDX-License-Identifier: Apache-2.0
//
// test_second_device_peer -- bytes one node stores in a region on the second device and the other node
// finds through its own mapping.
//
// The peer maps with no grant from the owner, as in test_mmap_peer. A mount of one device has no
// second device, so the case skips.
//
//   test_second_device_peer <mount-a> <mount-b>

#include <fcntl.h>
#include <sys/mman.h>

#include <cstdint>
#include <string>

#include "fs/file.hpp"
#include "harness/device.hpp"
#include "harness/harness.hpp"
#include "harness/mount.hpp"
#include "harness/pattern.hpp"

namespace
{

using fsuser::tests::caseName;
using fsuser::tests::PlacementSize;

constexpr std::uint64_t PlacedWords = PlacementSize / sizeof(std::uint32_t);
constexpr std::uint32_t OwnerSeed = 0x2D2D2D2DU;

void checkBytesCrossTheMounts(fsuser::tests::Report& report, fsuser::tests::Mount& owner,
                              fsuser::tests::Mount& peer, const std::string& name)
{
    auto file = fsuser::tests::placeOnDevice(owner, name, fsuser::tests::SecondDeviceAttr, PlacementSize);
    report.check("the region lies on device 1", fsuser::tests::allPiecesOn(name, fsuser::tests::SecondDevice));

    report.section("the owner stores a pattern");
    {
        auto stored = file.map(PlacementSize, PROT_READ | PROT_WRITE);
        report.check("the owner maps writably", stored.isMapped());
        fsuser::tests::fillPattern(static_cast<std::uint32_t*>(stored.get()), PlacedWords, OwnerSeed);
    }

    report.section("the peer reads it through the other mount");
    auto seen = peer.open(name, O_RDWR);
    auto reading = seen.map(PlacementSize, PROT_READ);
    report.check("the peer maps for reading", reading.isMapped());
    fsuser::tests::checkPattern(report, "the peer's mapping holds the owner's pattern",
                                static_cast<const std::uint32_t*>(reading.get()), PlacedWords, OwnerSeed);
}

}  // namespace

int main(int argc, char** argv)
{
    const auto body = [](fsuser::tests::Report& report, fsuser::tests::Mounts_t& mounts)
    {
        if (!fsuser::tests::secondDevicePool())
        {
            report.note(fsuser::tests::NoSecondDeviceNote);
            return;
        }

        const auto name = caseName("device2_peer");
        auto owner = fsuser::tests::Mount(mounts.first());
        auto peer = fsuser::tests::Mount(mounts.second());
        checkBytesCrossTheMounts(report, owner, peer, name);
        owner.unlink(name);
    };
    return fsuser::tests::runCase(argc, argv, "test_second_device_peer", 2, body);
}
