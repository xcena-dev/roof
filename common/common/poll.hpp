// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// poll.hpp -- waiting by looking again, for a condition no word bump announces.
//
// The futex waits elsewhere in this tree sleep on a word another thread stores to. What is here is
// for the rest: a mutex falling free, a counter reaching a mark, a child changing state. Nobody
// wakes the waiter, so the only wait available is to look, sleep, and look again.
//
// The step is a parameter with no default. A step short enough for a critical section ending is
// wrong for a process starting, and a default is what makes that choice by accident.

#pragma once

#include <thread>

#include "common/timing.hpp"

namespace polling
{

// Looks for @wanted until it holds or @within elapses, sleeping @step between looks.
//
// The look comes before the deadline test, so an answer that arrived during the last sleep is still
// taken. A clock and not a count of sleeps, because the look costs time too.
template <typename T_Check>
[[nodiscard]] bool waitUntil(T_Check wanted, timing::Nanos within, timing::Nanos step)
{
    const timing::Deadline waiting{within};

    while (true)
    {
        if (wanted())
        {
            return true;
        }

        if (waiting.expired())
        {
            return false;
        }

        std::this_thread::sleep_for(step);
    }
}

// The same wait where the look answers with a value rather than a predicate: the first non-empty
// answer, or an empty one on the deadline.
//
// @look returns an optional, and what that optional holds is what a caller could not take again
// after the look returned. A lock is the case that needs it.
template <typename T_Look>
[[nodiscard]] auto awaitValue(T_Look look, timing::Nanos within, timing::Nanos step) -> decltype(look())
{
    const timing::Deadline waiting{within};

    while (true)
    {
        auto found = look();
        if (found)
        {
            return found;
        }

        if (waiting.expired())
        {
            return {};
        }

        std::this_thread::sleep_for(step);
    }
}

}  // namespace polling
