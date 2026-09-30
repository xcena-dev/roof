// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/notify_test.cpp -- the readiness the unit's Type=notify waits for.
//
// The unit holds the start until READY=1 arrives, so a daemon that never sends it looks like a
// daemon that failed to come up. The cases below bind the socket themselves and read what lands.

#include "posix/notify.hpp"

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "harness/probe.hpp"
#include "posix/unique_fd.hpp"

namespace
{

using fsdaemon::posix::UniqueFd;

// A datagram socket bound to an abstract name of its own, which is the form systemd hands a unit.
// Abstract rather than a file so the case leaves nothing behind on a path.
class NotifySocket
{
public:
    explicit NotifySocket(std::string name)
        : name_{std::move(name)},
          held_{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)}
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(name_.begin(), name_.end(), static_cast<char*>(address.sun_path));
        address.sun_path[0] = '\0';
        const auto length = static_cast<::socklen_t>(offsetof(sockaddr_un, sun_path) + name_.size());
        bound_ = held_ && ::bind(held_.get(), reinterpret_cast<const sockaddr*>(&address), length) == 0;
    }

    [[nodiscard]] bool isBound() const noexcept
    {
        return bound_;
    }

    // What one datagram carried, or empty when none is waiting. The socket is left blocking, so a
    // case calls this only after the send it is reading.
    [[nodiscard]] std::string readOne() const
    {
        std::array<char, 64> buffer{};
        const auto got = ::recv(held_.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (got <= 0)
        {
            return {};
        }
        return std::string{buffer.data(), static_cast<std::string::size_type>(got)};
    }

private:
    std::string name_;
    UniqueFd held_;
    bool bound_{false};
};

void checkReadyNotification(fsdaemon::probe::Context& ctx)
{
    const std::string name = "@daemon-probe-notify-" + std::to_string(::getpid());
    const NotifySocket listening{name};
    if (!ctx.check(listening.isBound(), "a probe can stand in for the supervisor's socket"))
    {
        return;
    }

    ::setenv("NOTIFY_SOCKET", name.c_str(), 1);
    ctx.check(fsdaemon::posix::sendReady(), "the daemon reports itself ready");
    ctx.check(listening.readOne() == "READY=1", "what lands is what systemd waits for");
    ::unsetenv("NOTIFY_SOCKET");
}

void checkHandRunWithoutSupervisor(fsdaemon::probe::Context& ctx)
{
    ::unsetenv("NOTIFY_SOCKET");
    ctx.check(fsdaemon::posix::sendReady(), "no NOTIFY_SOCKET is a hand run and not a failure");
}

void checkUnreachableSocketFailure(fsdaemon::probe::Context& ctx)
{
    ::setenv("NOTIFY_SOCKET", "@daemon-probe-nobody-listens-here", 1);
    ctx.check(!fsdaemon::posix::sendReady(), "a socket nothing bound is reported as a failure");
    ::unsetenv("NOTIFY_SOCKET");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkReadyNotification(ctx);
            checkHandRunWithoutSupervisor(ctx);
            checkUnreachableSocketFailure(ctx);
        });
}
