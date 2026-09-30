// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/harness/socket_pair.hpp -- the two ends a probe uses instead of the real channel.
//
// One end goes to FsChannel, which closes it, and the other stays here as the kernel's side. Both
// are closed when this goes, so a case that fails halfway leaks no descriptor.

#pragma once

#include <sys/socket.h>

#include <array>

#include "posix/unique_fd.hpp"

namespace fsdaemon::probe
{

class SocketPair
{
public:
    SocketPair()
    {
        std::array<posix::Descriptor, 2> ends{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0)
        {
            theirs_ = posix::UniqueFd{ends[0]};
            mine_ = posix::UniqueFd{ends[1]};
        }
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    // Whether the pair was made. A case checks this before using either end.
    [[nodiscard]] bool isOpen() const noexcept
    {
        return static_cast<bool>(mine_) && static_cast<bool>(theirs_);
    }

    // The probe's end: what a case writes requests into and reads responses out of.
    [[nodiscard]] posix::Descriptor getMine() const noexcept
    {
        return mine_.get();
    }

    // The daemon's end, handed to FsChannel::makeChannel, which takes ownership. Called once.
    [[nodiscard]] posix::Descriptor releaseTheirs() noexcept
    {
        return theirs_.release();
    }

    // Closes the probe's end, which is how a case makes the daemon side read end of stream.
    void closeMine() noexcept
    {
        mine_ = posix::UniqueFd{};
    }

private:
    posix::UniqueFd mine_;
    posix::UniqueFd theirs_;
};

}  // namespace fsdaemon::probe
