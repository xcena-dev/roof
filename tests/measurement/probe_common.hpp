// SPDX-License-Identifier: Apache-2.0
//
// probe_common.hpp -- what the probes that run on plain syscalls share.
// They link nothing of the library, so this header names none of it either.

#pragma once

#include <sys/resource.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace fsuser::probes
{

constexpr std::uint64_t PageBytes = 4096;
constexpr std::uint64_t MiBBytes = 1024ULL * 1024ULL;

[[nodiscard]] inline std::optional<std::uint64_t> parseUnsigned(std::string_view text)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline std::uint64_t readMinorFaults()
{
    struct rusage usage = {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_minflt);
}

}  // namespace fsuser::probes
