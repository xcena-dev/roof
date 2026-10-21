// SPDX-License-Identifier: Apache-2.0
//
// slot.hpp -- a file's RAT slot as region_info shows it: which one it is, and whether it is still taken.
//
// A name can be gone while its slot stands, because a descriptor or a mapping somewhere keeps the
// extent. region_info is where that shows, so a case that asks "is the extent still held" reads the
// slot and not the name.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/sysfs.hpp"

namespace fsuser::tests
{

[[nodiscard]] inline std::optional<RegionRow_t> findRowByName(const std::string& name)
{
    for (auto& row : readRegionRows())
    {
        if (row.name == name)
        {
            return row;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool slotIsTaken(std::int64_t entry)
{
    for (auto& row : readRegionRows())
    {
        if (row.entry == entry)
        {
            return true;
        }
    }
    return false;
}

// How long the last holder's release has to reach the owner's sweep. Two sweep intervals, so a cycle
// already under way when the bit cleared cannot be the one this waits for.
constexpr timing::Secs SlotFreeDeadline{40};
constexpr timing::Millis SlotFreePoll{250};

// Bounded, so a sweep that never frees the slot ends the case with a failed check, not a hang.
[[nodiscard]] inline bool waitForSlotFree(std::int64_t entry)
{
    return polling::waitUntil(
        [entry]
        {
            return !slotIsTaken(entry);
        },
        SlotFreeDeadline, SlotFreePoll);
}

}  // namespace fsuser::tests
