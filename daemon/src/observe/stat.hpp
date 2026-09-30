// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// observe/stat.hpp -- where one answer spends its time inside this daemon.
//
// The kernel stamps a turn from the outside and reports it as one number, so what that number is
// made of is only visible from in here. This keeps a sum and a count per named stage and reports
// means, not a line per answer. Off unless a config asks, and off means no clock is read.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace fsdaemon::observe
{

class Stat
{
public:
    explicit Stat(bool enabled) noexcept
        : enabled_{enabled}
    {
    }

    Stat(const Stat&) = delete;
    Stat& operator=(const Stat&) = delete;

    // A start stamp, or 0 when this is off so a caller pays nothing for asking.
    [[nodiscard]] std::uint64_t mark() const noexcept
    {
        return enabled_ ? readClock() : 0;
    }

    // Folds the time since @beganNs into @stage and returns a stamp for the next one. Workers and
    // the loop both fold, so this is locked.
    std::uint64_t add(std::string_view stage, std::uint64_t beganNs)
    {
        if (!enabled_ || beganNs == 0)
        {
            return 0;
        }
        const auto now = readClock();
        const std::lock_guard guard{guard_};
        auto& totals = byStage_[std::string{stage}];
        totals.nanoseconds += now - beganNs;
        ++totals.count;
        return now;
    }

    // One answer finished. Reports the means and starts over once enough have.
    void answered()
    {
        if (!enabled_)
        {
            return;
        }
        std::string line;
        {
            const std::lock_guard guard{guard_};
            ++answers_;
            if (answers_ < ReportEvery)
            {
                return;
            }
            constexpr auto NanosecondsPerMicrosecond = 1000.0;
            for (const auto& [stage, totals] : byStage_)
            {
                const auto mean = static_cast<double>(totals.nanoseconds) /
                                  static_cast<double>(totals.count) / NanosecondsPerMicrosecond;
                line.append(line.empty() ? "" : " ").append(stage).append("=");
                line.append(std::to_string(mean)).append("us");
            }
            byStage_.clear();
            answers_ = 0;
        }
        std::fprintf(stderr, "observe %s\n", line.c_str());
    }

    // How many folds @stage has taken since the last report. The report itself goes to stderr, so
    // this is what a probe reads.
    [[nodiscard]] std::uint64_t getFoldCount(std::string_view stage) const
    {
        const std::lock_guard guard{guard_};
        const auto found = byStage_.find(std::string{stage});
        return found == byStage_.end() ? 0 : found->second.count;
    }

    // How many answers have finished since the last report.
    [[nodiscard]] std::uint64_t getAnswerCount() const
    {
        const std::lock_guard guard{guard_};
        return answers_;
    }

private:
    // Answers between reports. Large enough that reporting costs nothing next to the work, small
    // enough that one probe run says something.
    static constexpr std::uint64_t ReportEvery = 200;

    struct Totals_t
    {
        std::uint64_t nanoseconds{0};
        std::uint64_t count{0};
    };

    [[nodiscard]] static std::uint64_t readClock() noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool enabled_;
    mutable std::mutex guard_;
    // Ordered, so a report reads the same way every time.
    std::map<std::string, Totals_t> byStage_;
    std::uint64_t answers_{0};
};

// The three calls above with a null @stat meaning an uninstrumented run. Every holder of a recorder
// holds it as a pointer that may be null, so the check belongs here rather than at each of them.

[[nodiscard]] inline std::uint64_t markStart(const Stat* stat) noexcept
{
    return stat == nullptr ? 0 : stat->mark();
}

inline std::uint64_t addStage(Stat* stat, std::string_view stage, std::uint64_t beganNs)
{
    return stat == nullptr ? 0 : stat->add(stage, beganNs);
}

inline void recordAnswer(Stat* stat)
{
    if (stat != nullptr)
    {
        stat->answered();
    }
}

}  // namespace fsdaemon::observe
