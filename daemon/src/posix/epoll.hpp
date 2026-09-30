// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// posix/epoll.hpp -- one wait over every descriptor the serve loop cares about.
//
// The device, the stop eventfd and the reload eventfd all go in, so the loop blocks on one wait and
// answers whichever is ready. EINTR needs no branch: an interrupted wait reads as nothing ready, and
// the next pass sees whatever a handler left behind. A ready descriptor names itself, so no table
// sits beside the epoll.

#pragma once

#include <sys/epoll.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

#include "posix/unique_fd.hpp"

namespace fsdaemon::posix
{

class Epoll
{
public:
    explicit Epoll(std::uint32_t atOnce)
        : held_{::epoll_create1(EPOLL_CLOEXEC)},
          filled_(atOnce)
    {
        if (!held_)
        {
            throw std::system_error{errno, std::system_category(), "epoll_create1"};
        }
    }

    // Adds @descriptor to the wait for the readable event. The descriptor is its own token, carried
    // back on a wake.
    void watchReadable(Descriptor descriptor)
    {
        ::epoll_event wanted{};
        wanted.events = EPOLLIN;
        wanted.data.u64 = static_cast<std::uint64_t>(descriptor);
        if (::epoll_ctl(held_.get(), EPOLL_CTL_ADD, descriptor, &wanted) != 0)
        {
            throw std::system_error{errno, std::system_category(), "epoll_ctl(add)"};
        }
    }

    // The descriptors ready, empty on a signal. Waits for as long as it takes when @timeout is
    // nullopt, and at most that long otherwise. EPOLLIN is the only event asked for, so a wake
    // carries its descriptor and nothing a caller reads besides.
    //
    // epoll_pwait2 and not epoll_wait: its timeout is a duration rather than whole milliseconds, so
    // a caller may ask for less than one and get what the timer can actually give.
    [[nodiscard]] std::vector<Descriptor> wait(std::optional<std::chrono::nanoseconds> timeout = std::nullopt)
    {
        ::timespec waiting{};
        if (timeout)
        {
            const auto whole = std::chrono::duration_cast<std::chrono::seconds>(*timeout);
            waiting.tv_sec = static_cast<::time_t>(whole.count());
            // NOLINTNEXTLINE(google-runtime-int) tv_nsec is long, and POSIX is what names it
            waiting.tv_nsec = static_cast<long>((*timeout - whole).count());
        }
        const std::int32_t seen = ::epoll_pwait2(held_.get(), filled_.data(),
                                                 static_cast<int>(filled_.size()),
                                                 timeout ? &waiting : nullptr, nullptr);
        std::vector<Descriptor> ready;
        if (seen < 0)
        {
            if (errno == EINTR)
            {
                return ready;
            }
            throw std::system_error{errno, std::system_category(), "epoll_pwait2"};
        }
        ready.reserve(static_cast<std::size_t>(seen));
        std::transform(filled_.begin(), std::next(filled_.begin(), seen), std::back_inserter(ready),
                       [](const ::epoll_event& woke)
                       {
                           return static_cast<Descriptor>(woke.data.u64);
                       });
        return ready;
    }

private:
    UniqueFd held_;
    std::vector<::epoll_event> filled_;
};

}  // namespace fsdaemon::posix
