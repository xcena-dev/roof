// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// posix/event_fd.hpp -- a descriptor another thread or a signal handler pokes to wake an epoll wait.
//
// The serve loop sleeps in epoll_wait. A flag would need the loop to wake on its own to notice; an
// eventfd is a descriptor the wait already watches, so one write ends the sleep at once.

#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

#include "posix/unique_fd.hpp"

namespace fsdaemon::posix
{

class EventFd
{
public:
    EventFd()
        : held_{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)}
    {
        if (!held_)
        {
            throw std::system_error{errno, std::system_category(), "eventfd"};
        }
    }

    [[nodiscard]] Descriptor getDescriptor() const noexcept
    {
        return held_.get();
    }

    // Wakes whoever waits on this descriptor. Safe from a signal handler: it is one write and nothing
    // else. A full counter is the wake already pending, so a failed write needs no handling.
    void poke() const noexcept
    {
        const std::uint64_t one = 1;
        const std::int64_t put = ::write(held_.get(), &one, sizeof(one));
        static_cast<void>(put);
    }

    // True when @descriptor is this one, and then the counter is cleared so the next wait sleeps
    // again. False leaves it alone, so a caller cannot clear one eventfd on another's wake.
    [[nodiscard]] bool consumeWake(Descriptor descriptor) const noexcept
    {
        if (descriptor != held_.get())
        {
            return false;
        }
        std::uint64_t sink = 0;
        const std::int64_t got = ::read(held_.get(), &sink, sizeof(sink));
        static_cast<void>(got);
        return true;
    }

private:
    UniqueFd held_;
};

}  // namespace fsdaemon::posix
