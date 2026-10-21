// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// timing.hpp -- the clock this tree measures with, and the two things it does with one.
//
// A budget that runs out, and a span that is measured after the fact. Kept apart because they are
// asked opposite questions: one is how much is left, the other is how much went by.
//
// Everything here reads the clock through now() and nowhere else, so which clock that is stays one
// line rather than a habit spread over the tree.
//
// Header-only and free of both libraries, so the library, the daemon, the tools and the harness all
// take the same ones, and a wait written in one of them reads the same as a wait written in another.

#pragma once

// The aliases below are this header's surface, and a caller naming Millis is not naming
// std::chrono::milliseconds. Exported so include-cleaner attributes them here rather than sending
// every consumer to <chrono> for a type it never spells.
// timespec and timeval are here for the same reason getTicks is: a kernel interface takes a duration
// as its own shape, and every caller converting by hand is every caller getting the remainder wrong.
#include <sys/time.h>  // IWYU pragma: export

#include <chrono>  // IWYU pragma: export
#include <cstdint>
#include <ctime>
#include <optional>
#include <ratio>
#include <type_traits>

namespace timing
{

// steady_clock and not system_clock: a span measured across a clock adjustment is not a span, and a
// deadline that moved with one is not a deadline. A cross-node witness needs wall time instead,
// which is a different question and a different function (wall, below).
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

using Nanos = std::chrono::nanoseconds;
using Micros = std::chrono::microseconds;
using Millis = std::chrono::milliseconds;
using Secs = std::chrono::seconds;

// Fractional, for a report that wants the part below the unit. The integral ones above truncate it
// away, and a caller that divided an integral count by 1000 to get there lost it before dividing.
using NanosF = std::chrono::duration<double, std::nano>;
using MicrosF = std::chrono::duration<double, std::micro>;
using MillisF = std::chrono::duration<double, std::milli>;
using SecsF = std::chrono::duration<double>;

// A nanosecond count that is already a double, such as a mean or a percentile, in microseconds.
[[nodiscard]] constexpr double toMicros(double nanos) noexcept
{
    return MicrosF{NanosF{nanos}}.count();
}

// A duration as the raw count a shared word or a C interface in that unit takes. constexpr, so a
// configuration constant converts once at compile time instead of on every comparison against it.
//
// No default unit here, nor on monotonic and wall below, unlike Deadline::remaining and
// Stopwatch::elapsed. Those return a duration, which carries its unit in the type and makes a
// mismatch a compile error. These return a bare count, and the words they are written into are read
// by another node as a fixed unit, so the caller has to say which one out loud.
// A floating target is refused rather than counted: the count is an integer, so the fraction the
// caller asked for by naming SecsF would be floored away without a word. Construct that type instead.
template <typename T_Duration, typename Rep, typename Period>
[[nodiscard]] constexpr std::uint64_t getTicks(std::chrono::duration<Rep, Period> value) noexcept
{
    static_assert(std::is_integral_v<typename T_Duration::rep>,
                  "getTicks answers an integer count; construct a floating duration instead");
    return static_cast<std::uint64_t>(std::chrono::duration_cast<T_Duration>(value).count());
}

// A duration as the two-field shapes the kernel interfaces take: timespec for futex and nanosleep,
// timeval for the socket timeouts.
//
// The sub-second field is the remainder of a subtraction and not a second cast of the whole span.
// Mixing periods lands in the finer of the two, so the ratio between them stays in the types instead
// of being written out as a constant that has to agree with them.
//
// decltype on the members rather than a named width: tv_sec and tv_nsec are not the same type
// everywhere.
template <typename Rep, typename Period>
[[nodiscard]] constexpr ::timespec toTimespec(std::chrono::duration<Rep, Period> span) noexcept
{
    const Secs seconds = std::chrono::duration_cast<Secs>(span);
    ::timespec value = {};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(seconds.count());
    value.tv_nsec = static_cast<decltype(value.tv_nsec)>(getTicks<Nanos>(span - seconds));
    return value;
}

template <typename Rep, typename Period>
[[nodiscard]] constexpr ::timeval toTimeval(std::chrono::duration<Rep, Period> span) noexcept
{
    const Secs seconds = std::chrono::duration_cast<Secs>(span);
    ::timeval value = {};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(seconds.count());
    value.tv_usec = static_cast<decltype(value.tv_usec)>(getTicks<Micros>(span - seconds));
    return value;
}

[[nodiscard]] inline TimePoint now() noexcept
{
    return std::chrono::steady_clock::now();
}

// Since an arbitrary fixed origin, so only differences between two of these mean anything, and only
// between two taken on the same machine. For a timeline that stores stamps and subtracts them
// later, where a Stopwatch cannot reach.
template <typename T_Duration>
[[nodiscard]] inline std::uint64_t monotonic() noexcept
{
    return getTicks<T_Duration>(now().time_since_epoch());
}

// The other clock, and the only one a stamp another node wrote can be compared against: its origin
// is the Unix epoch rather than this boot. Steppable, so it is wrong for a span or a budget.
// Cross-node comparisons hold under the declared skew bound (HEARTBEAT design §2.4).
template <typename T_Duration>
[[nodiscard]] inline std::uint64_t wall() noexcept
{
    return getTicks<T_Duration>(std::chrono::system_clock::now().time_since_epoch());
}

// The stamp to write for an instant @ahead from now, in the nanoseconds a shared layout fixes. The one
// place the widening happens, and a span already past stamps now rather than underflowing into 2^64.
template <typename T_Duration>
[[nodiscard]] inline std::uint64_t wallAfter(T_Duration ahead) noexcept
{
    const auto span = std::chrono::duration_cast<Nanos>(ahead);
    const std::uint64_t stamped = wall<Nanos>();

    return span <= Nanos::zero() ? stamped : stamped + static_cast<std::uint64_t>(span.count());
}

// A wall-clock instant somebody else stamped, and the two questions worth asking of one.
//
// Not a Deadline. A Deadline is anchored at the moment this process built it, and this anchor was
// chosen by another node and is re-read from shared memory on every look.
//
// The trap it exists to name is the subtraction. A peer whose clock runs ahead of ours leaves a
// stamp in our future, and taking now - stamp there underflows into an age of nearly 2^64 ns, which
// reads as long dead. Both answers below are optional so that case cannot be subtracted by accident.
//
// Nanoseconds, not a unit parameter, unlike monotonic and wall above. Those produce a count and the
// destination picks the unit; this takes one whose unit the shared layout already fixed. A unit
// parameter here would only add a way to read that word off by a factor of a million. The answers
// come back as durations, so a caller still compares them against a constant in any unit.
class WallStamp
{
public:
    explicit constexpr WallStamp(std::uint64_t nanos) noexcept
        : nanos_{nanos}
    {
    }

    // Never stamped, which is not the same as stamped long ago. What that means is the caller's:
    // an operator view has no age to show, while a liveness check may well treat it as dead.
    [[nodiscard]] constexpr bool isUnset() const noexcept
    {
        return nanos_ == 0;
    }

    // How long ago, as of an instant the caller already read, or nullopt when the stamp is at or
    // ahead of it. The instant is a parameter for the same reason Deadline::expiredAt takes one: a
    // view that walks every peer reads its clock once, so that all of them are judged against it.
    [[nodiscard]] constexpr std::optional<Nanos> ageAt(std::uint64_t nowNanos) const noexcept
    {
        if (nowNanos <= nanos_)
        {
            return std::nullopt;
        }
        return Nanos{static_cast<Nanos::rep>(nowNanos - nanos_)};
    }

    // How far into that instant's future it sits, or nullopt when it is not ahead. Small values are
    // sampling order rather than disagreement, since the peers are read one after another.
    [[nodiscard]] constexpr std::optional<Nanos> aheadAt(std::uint64_t nowNanos) const noexcept
    {
        if (nanos_ <= nowNanos)
        {
            return std::nullopt;
        }
        return Nanos{static_cast<Nanos::rep>(nanos_ - nowNanos)};
    }

    // The same two against the clock right now, for a caller asking about one stamp.
    [[nodiscard]] std::optional<Nanos> age() const noexcept
    {
        return ageAt(wall<Nanos>());
    }

    [[nodiscard]] std::optional<Nanos> ahead() const noexcept
    {
        return aheadAt(wall<Nanos>());
    }

private:
    std::uint64_t nanos_;
};

// A caller is given a duration and needs an instant, because every wait after the first has to ask
// how much of the budget is left rather than restart it.
class Deadline
{
public:
    template <typename Rep, typename Period>
    explicit Deadline(std::chrono::duration<Rep, Period> timeout) noexcept
        : ends_{now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)}
    {
    }

    // nullopt when the budget is spent, so a spent deadline is not a duration at all. Returning
    // zero would be: a caller that passed it on would ask a syscall not to wait, which is a
    // different request from the one that ran out of time.
    //
    // The test is on the converted value, not the raw one. duration_cast truncates toward zero, so
    // half a millisecond asked for in ms is zero, and a caller that passed that zero on would spin
    // instead of giving up. Less than one of the unit asked for is spent, in that unit.
    template <typename T_Duration = Nanos>
    [[nodiscard]] std::optional<T_Duration> remaining() const noexcept
    {
        const auto left = std::chrono::duration_cast<T_Duration>(ends_ - now());

        if (left <= T_Duration::zero())
        {
            return std::nullopt;
        }

        return left;
    }

    // For a caller that only needs the predicate. One that needs the value takes remaining() once
    // and branches on it: asking both reads the clock twice and gets two different instants.
    [[nodiscard]] bool expired() const noexcept
    {
        return now() >= ends_;
    }

    // Past, as of an instant the caller already read. For a spin loop that asks two deadlines the
    // same question: one clock read serves both, and asking each of them separately would double
    // the reads in the hottest loop there is.
    [[nodiscard]] bool expiredAt(TimePoint instant) const noexcept
    {
        return instant >= ends_;
    }

    // The instant itself, for an interface that takes one rather than a budget.
    [[nodiscard]] TimePoint endsAt() const noexcept
    {
        return ends_;
    }

private:
    TimePoint ends_;
};

class Stopwatch
{
public:
    Stopwatch() noexcept
        : began_{now()}
    {
    }

    // For a span whose start is later than the object's, such as a log origin re-based past setup.
    void restart() noexcept
    {
        began_ = now();
    }

    template <typename T_Duration = Nanos>
    [[nodiscard]] T_Duration elapsed() const noexcept
    {
        return std::chrono::duration_cast<T_Duration>(now() - began_);
    }

private:
    TimePoint began_;
};

// The due time of one piece of periodic work. isDue is true once per period of the instants passed
// in, then rearms, and starts due. Taking the instant lets every gate in one pass share a clock read.
class PeriodicGate
{
public:
    [[nodiscard]] bool isDue(TimePoint instant, Nanos period) noexcept
    {
        if (instant < next_)
        {
            return false;
        }
        next_ = instant + period;
        return true;
    }

private:
    TimePoint next_{};
};

}  // namespace timing