// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/harness/wire_io.hpp -- whole frames in and out of a probe's end of the channel.
//
// A socket hands back what it has, not what was asked for, so reading one response means looping.
// Doing that inline is how three copies of the same loop appeared. A case says which response it
// expects and reads the seq and the payload off what comes back.

#pragma once

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "posix/unique_fd.hpp"
#include "wire/codec.hpp"

namespace fsdaemon::probe
{

// One response as a case reads it: the seq that matches it to a request, and the decoded payload.
template <typename T>
struct Response_t
{
    std::uint64_t seq{0};
    T payload{};
};

// Writes every byte of @bytes, or stops at the first refusal. A short write means the peer is gone,
// which the case then sees as a response that never arrives.
inline void writeAll(posix::Descriptor descriptor, wire::ByteView bytes)
{
    std::uint64_t sent = 0;
    while (sent < bytes.size())
    {
        const auto put = ::write(descriptor, bytes.data() + sent, bytes.size() - sent);
        if (put <= 0)
        {
            return;
        }
        sent += static_cast<std::uint64_t>(put);
    }
}

// Reads @count bytes, or fewer when the peer stops. A case reads the size back to tell the two
// apart, since a short read is itself an outcome some cases assert.
[[nodiscard]] inline std::vector<std::byte> readExactly(posix::Descriptor descriptor,
                                                        std::uint64_t count)
{
    std::vector<std::byte> got(count);
    std::uint64_t have = 0;
    while (have < count)
    {
        const auto read = ::read(descriptor, got.data() + have, count - have);
        if (read <= 0)
        {
            got.resize(have);
            break;
        }
        have += static_cast<std::uint64_t>(read);
    }
    return got;
}

// Puts one request frame of @type and @seq on the wire, header and payload together.
template <typename T>
void writeFrame(posix::Descriptor descriptor, std::uint32_t type, std::uint64_t seq, const T& payload)
{
    const auto payloadBytes = wire::encode(payload);
    writeAll(descriptor, wire::makeFrame(type, seq, payloadBytes));
}

// One whole response frame of type @T, or nullopt when the peer sent too little, the header will
// not parse, or the type is not @expectedType.
template <typename T>
[[nodiscard]] std::optional<Response_t<T>> readResponse(posix::Descriptor descriptor,
                                                        std::uint32_t expectedType)
{
    const auto back = readExactly(descriptor, wire::HeaderBytes + sizeof(T));
    if (back.size() < wire::HeaderBytes + sizeof(T))
    {
        return std::nullopt;
    }
    const auto header = wire::parseHeader(back);
    if (!header || header->type != expectedType)
    {
        return std::nullopt;
    }
    const auto payload =
        wire::decode<T>(wire::ByteView{back.data() + wire::HeaderBytes, sizeof(T)});
    if (!payload)
    {
        return std::nullopt;
    }
    return Response_t<T>{header->seq, *payload};
}

}  // namespace fsdaemon::probe
