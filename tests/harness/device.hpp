// SPDX-License-Identifier: Apache-2.0
//
// device.hpp -- placing a region on a chosen device, and finding out whether the mount has a second.
//
// A case about the second device asks secondDevicePool first and reports a skip when it is empty, so
// the same binary passes on a mount of one device.

#pragma once

#include <fcntl.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "fs/file.hpp"
#include "harness/mount.hpp"
#include "harness/sysfs.hpp"

namespace fsuser::tests
{

// Device indices as pool_info and extents number them. RegionAttr_t::device is 1 + the index.
constexpr std::uint32_t PrimaryDevice = 0;
constexpr std::uint32_t SecondDevice = 1;
constexpr std::uint32_t PrimaryDeviceAttr = PrimaryDevice + 1;
constexpr std::uint32_t SecondDeviceAttr = SecondDevice + 1;

constexpr const char* NoSecondDeviceNote = "the mount has one device, so there is no second device to place on";

// pool_info when it lists a second device, or nullopt on a mount of one.
[[nodiscard]] inline std::optional<PoolInfo_t> secondDevicePool()
{
    auto pools = readPoolInfo();
    if (!pools || pools->devices.size() <= SecondDevice)
    {
        return std::nullopt;
    }
    return pools;
}

// A region attribute that asks for @device and leaves the rest to the caller.
[[nodiscard]] inline RegionAttr_t makeDeviceAttr(std::uint32_t device, CachePolicy cache = CachePolicy::Writeback,
                                                 Placement placement = Placement::Contiguous)
{
    return RegionAttr_t{cache, placement, 0, device};
}

// Created, asked onto @device, and placed to @bytes.
[[nodiscard]] inline File placeOnDevice(Mount& mount, const std::string& name, std::uint32_t device,
                                        std::uint64_t bytes, Placement placement = Placement::Contiguous)
{
    auto file = mount.open(name, O_CREAT | O_RDWR, 0644);
    file.setRegionAttr(makeDeviceAttr(device, CachePolicy::Writeback, placement));
    file.resize(bytes);
    return file;
}

// Every piece of @name lies on device @device, and there is at least one.
[[nodiscard]] inline bool allPiecesOn(const std::string& name, std::uint32_t device)
{
    const auto pieces = readPiecesOf(name);
    return !pieces.empty() && std::all_of(pieces.begin(), pieces.end(), [device](const Piece_t& piece)
                                          {
                                              return piece.device == device;
                                          });
}

}  // namespace fsuser::tests
