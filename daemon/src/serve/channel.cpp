// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/channel.cpp -- see channel.hpp.

#include "serve/channel.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "observe/stat.hpp"
#include "posix/unique_fd.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace fsdaemon::serve
{

using wire::HeaderBytes;

namespace
{

// Sets a descriptor non-blocking, so a read takes what is ready and stops rather than parking the
// loop thread on the channel.
void makeNonBlocking(posix::Descriptor descriptor)
{
    const auto flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        throw std::system_error{errno, std::system_category(), "fcntl(O_NONBLOCK)"};
    }
}

}  // namespace

FsChannel FsChannel::makeChannel(const std::string& path)
{
    posix::UniqueFd held{::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)};
    if (!held)
    {
        throw std::system_error{errno, std::system_category(), "open " + path};
    }
    return FsChannel{std::move(held)};
}

FsChannel FsChannel::makeChannel(posix::Descriptor descriptor)
{
    makeNonBlocking(descriptor);
    return FsChannel{posix::UniqueFd{descriptor}};
}

std::vector<Frame_t> FsChannel::readReady()
{
    const auto began = observe::markStart(stat_);
    std::vector<Frame_t> frames;

    // Only a loop bound, not a limit: the loop below reads until the descriptor would block, so a
    // burst larger than this costs another read rather than being cut.
    constexpr std::uint64_t ChunkBytes = 4ULL * 1024;
    std::array<std::byte, ChunkBytes> chunk{};
    for (;;)
    {
        const std::int64_t got = ::read(getDescriptor(), chunk.data(), chunk.size());
        if (got > 0)
        {
            pending_.insert(pending_.end(), chunk.begin(), chunk.begin() + got);
            continue;
        }
        if (got == 0)
        {
            endOfStream_ = true;
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        // Any other read error is the channel gone, which the loop treats like end of stream.
        endOfStream_ = true;
        break;
    }

    std::uint64_t offset = 0;
    while (pending_.size() - offset >= HeaderBytes)
    {
        const wire::ByteView head{pending_.data() + offset, HeaderBytes};
        const auto header = wire::parseHeader(head);
        if (!header || header->payload_len > FS_DAEMON_MAX_PAYLOAD_BYTES)
        {
            // A stream cannot be resynced past a frame of unknown length, so drop what is buffered;
            // the frames already extracted above still stand.
            pending_.clear();
            return frames;
        }
        const auto frameLen = HeaderBytes + header->payload_len;
        if (pending_.size() - offset < frameLen)
        {
            break;
        }
        const auto* payloadStart = pending_.data() + offset + HeaderBytes;
        frames.push_back(Frame_t{*header, {payloadStart, payloadStart + header->payload_len}});
        offset += frameLen;
    }

    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    observe::addStage(stat_, "read", began);
    return frames;
}

bool FsChannel::sendFrame(std::uint32_t type, std::uint64_t seq, wire::ByteView payload)
{
    const auto buffer = wire::makeFrame(type, seq, payload);

    const std::lock_guard guard{writing_};
    std::uint64_t written = 0;
    while (written < buffer.size())
    {
        const std::int64_t put = ::write(getDescriptor(), buffer.data() + written, buffer.size() - written);
        if (put > 0)
        {
            written += static_cast<std::uint64_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR)
        {
            continue;
        }
        if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd waiting{getDescriptor(), POLLOUT, 0};  // NOLINT(misc-include-cleaner) <poll.h> is the public header for pollfd and POLLOUT, already included above
            constexpr std::int32_t WaitForever = -1;
            ::poll(&waiting, 1, WaitForever);  // NOLINT(misc-include-cleaner) poll's public header is <poll.h>, already included above
            continue;
        }
        return false;
    }
    return true;
}

bool FsChannel::sendHello(std::uint64_t capabilities, std::uint32_t helperPid)
{
    FsDaemonHello hello{};
    hello.protocol_version = FS_DAEMON_PROTOCOL_VERSION;
    hello.reserved = 0;
    hello.capabilities = capabilities;
    hello.helper_pid = helperPid;
    hello.reserved2 = 0;
    const auto bytes = wire::encode(hello);
    return sendFrame(FS_DAEMON_MSG_HELLO, 0, bytes);
}

bool FsChannel::sendAttestResponse(std::uint64_t seq, std::int32_t status, std::string_view group,
                                   std::string_view role)
{
    FsDaemonAttestResponse response{};
    response.status = status;
    response.reserved = 0;
    wire::writeName(response.group, group);
    wire::writeName(response.role, role);
    const auto bytes = wire::encode(response);
    return sendFrame(FS_DAEMON_MSG_ATTEST_RESPONSE, seq, bytes);
}

bool FsChannel::sendAccessResponse(std::uint64_t seq, std::int32_t status, std::uint32_t grantedPerms,
                                   std::int64_t expirySecs)
{
    FsDaemonResponse response{};
    response.status = status;
    response.granted_perms = grantedPerms;
    response.expiry_secs = expirySecs;
    response.reserved[0] = 0;
    response.reserved[1] = 0;
    const auto bytes = wire::encode(response);
    return sendFrame(FS_DAEMON_MSG_ACCESS_RESPONSE, seq, bytes);
}

bool FsChannel::sendLockResponse(std::uint64_t seq, std::uint32_t type, std::int32_t status)
{
    FsDaemonLockResponse response{};
    response.status = status;
    response.reserved = 0;
    const auto bytes = wire::encode(response);
    return sendFrame(type, seq, bytes);
}

}  // namespace fsdaemon::serve
