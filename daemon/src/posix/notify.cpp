// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// posix/notify.cpp -- see notify.hpp.

#include "posix/notify.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "posix/unique_fd.hpp"

namespace fsdaemon::posix
{

bool sendReady() noexcept
{
    const auto* const socketPath = std::getenv("NOTIFY_SOCKET");
    if (socketPath == nullptr || *socketPath == '\0')
    {
        return true;
    }

    const std::string_view path{socketPath};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    // One byte for the terminator the abstract form replaces with a leading NUL.
    if (path.size() >= sizeof(address.sun_path))
    {
        return false;
    }
    std::copy(path.begin(), path.end(), static_cast<char*>(address.sun_path));
    // '@' is how the variable spells an abstract socket, whose name starts with a NUL instead.
    if (address.sun_path[0] == '@')
    {
        address.sun_path[0] = '\0';
    }

    const UniqueFd socket{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (!socket)
    {
        return false;
    }
    // The abstract name carries its leading NUL, so the length is the family plus the bytes copied
    // and not what strlen would say.
    const auto addressLength = static_cast<::socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());

    constexpr std::string_view Ready = "READY=1";
    const std::int64_t sent = ::sendto(socket.get(), Ready.data(), Ready.size(), MSG_NOSIGNAL,
                                       reinterpret_cast<const sockaddr*>(&address), addressLength);
    return sent > 0 && static_cast<std::uint64_t>(sent) == Ready.size();
}

}  // namespace fsdaemon::posix
