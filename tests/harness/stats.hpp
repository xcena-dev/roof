// SPDX-License-Identifier: Apache-2.0
//
// stats.hpp -- the five numbers every measurement probe reports.
//
// Each probe prints its own CSV row, because they carry different columns. What they do not differ
// on is how the row's numbers are reached, so the sort and the percentile arithmetic live here.

#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace fsuser::tests
{

struct Summary_t
{
    std::uint64_t count{0};
    double least{0};
    double median{0};
    double p95{0};
    double most{0};
    double mean{0};
};

// The summary of @samples, sorted in place, or nullopt when there are none. A caller that measured
// nothing prints its own empty row: reading front() off an empty vector is what the nullopt
// prevents.
[[nodiscard]] inline std::optional<Summary_t> summarise(std::vector<double>& samples)
{
    if (samples.empty())
    {
        return std::nullopt;
    }
    std::sort(samples.begin(), samples.end());

    Summary_t summary;
    summary.count = samples.size();
    summary.least = samples.front();
    summary.median = samples[samples.size() / 2];
    summary.p95 = samples[(samples.size() * 95) / 100];
    summary.most = samples.back();
    summary.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                   static_cast<double>(samples.size());
    return summary;
}

}  // namespace fsuser::tests
