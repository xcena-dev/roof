// SPDX-License-Identifier: Apache-2.0
//
// pattern.hpp -- writing bytes into a region and finding them again.
//
// The pattern is keyed on the word's own index rather than being one repeated value. A mapping that
// landed at the wrong offset then reads as wrong words, where a constant fill would read as correct
// ones.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "harness/harness.hpp"

namespace fsuser::tests
{

[[nodiscard]] inline std::uint32_t patternWord(std::uint32_t seed, std::uint64_t index)
{
    return seed ^ static_cast<std::uint32_t>(index);
}

inline void fillPattern(std::uint32_t* base, std::uint64_t words, std::uint32_t seed)
{
    for (std::uint64_t index = 0; index < words; ++index)
    {
        base[index] = patternWord(seed, index);
    }
}

// One check for the whole span, naming the first word that disagrees along with both values. A bare
// count of mismatches would not say whether the mapping missed the bytes or found them shifted.
inline void checkPattern(Report& report, std::string_view what, const std::uint32_t* base,
                         std::uint64_t words, std::uint32_t seed)
{
    for (std::uint64_t index = 0; index < words; ++index)
    {
        const std::uint32_t wanted = patternWord(seed, index);
        if (base[index] != wanted)
        {
            report.check(std::string{what} + ": word " + std::to_string(index) + " reads " +
                             std::to_string(base[index]) + ", not " + std::to_string(wanted),
                         false);
            return;
        }
    }
    report.check(what, true);
}

}  // namespace fsuser::tests
