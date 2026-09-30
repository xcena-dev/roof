// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// wire/codec.hpp -- bytes on the device fd, to and from the packed structs daemon_uapi.h defines.
//
// Every message is a 32-byte fs_daemon_hdr and a payload. The structs are packed and same-endian on
// this host, so a frame is the struct's own bytes: decode is a bounds check and a memcpy, encode is
// the bytes back. The only shaping is the fixed-width name fields, which are NUL-padded going out
// and cut at the first NUL coming in.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "wire/protocol.hpp"

namespace fsdaemon::wire
{

inline constexpr std::uint64_t HeaderBytes = sizeof(FsDaemonHdr);
static_assert(HeaderBytes == 32, "fs_daemon_hdr is 32 bytes on the wire");

// A read-only run of bytes: a pointer and a length, which is the whole of what this layer asks of
// the buffer a frame arrived in. The container constructor takes only a container of bytes.
class ByteView
{
public:
    constexpr ByteView(const std::byte* first, std::uint64_t count) noexcept
        : first_{first},
          count_{count}
    {
    }

    template <typename T_Bytes,
              typename = std::enable_if_t<std::is_same_v<typename T_Bytes::value_type, std::byte>>>
    constexpr ByteView(const T_Bytes& bytes) noexcept
        : first_{bytes.data()},
          count_{bytes.size()}
    {
    }

    // Spelled as the standard library spells them, because a range-for and the std algorithms bind
    // to these names.
    [[nodiscard]] constexpr const std::byte* data() const noexcept
    {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept
    {
        return count_;
    }
    [[nodiscard]] constexpr const std::byte* begin() const noexcept
    {
        return first_;
    }
    [[nodiscard]] constexpr const std::byte* end() const noexcept
    {
        return first_ + count_;
    }

private:
    const std::byte* first_;
    std::uint64_t count_;
};

// One trivially-copyable message struct out of a byte run, or nullopt when the run is shorter than
// the struct. The bytes past sizeof(T) are ignored, so a payload the kernel padded still reads.
template <typename T>
[[nodiscard]] std::optional<T> decode(ByteView buffer) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "a wire struct is copied by its bytes");
    if (buffer.size() < sizeof(T))
    {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, buffer.data(), sizeof(T));
    return value;
}

// The bytes of one message struct, for writing a whole frame in one call.
template <typename T>
[[nodiscard]] std::array<std::byte, sizeof(T)> encode(const T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "a wire struct is copied by its bytes");
    std::array<std::byte, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

// The header in front of a payload of @payloadLen bytes, of message @type, matched to a request by
// @seq. A response reuses the request's seq so the kernel pairs it.
[[nodiscard]] inline FsDaemonHdr makeHeader(std::uint32_t type, std::uint64_t seq,
                                            std::uint32_t payloadLen) noexcept
{
    FsDaemonHdr header{};
    header.magic = FS_DAEMON_MAGIC;
    header.version = FS_DAEMON_PROTOCOL_VERSION;
    header.type = type;
    header.payload_len = payloadLen;
    header.seq = seq;
    header.reserved[0] = 0;
    header.reserved[1] = 0;
    return header;
}

// A header parsed off the front of a frame, once its magic and version are the ones this daemon
// answers. nullopt for a frame too short, a wrong magic, or a version outside the accepted window,
// which is every reason to drop a frame rather than answer it.
[[nodiscard]] inline std::optional<FsDaemonHdr> parseHeader(ByteView buffer) noexcept
{
    auto header = decode<FsDaemonHdr>(buffer);
    if (!header)
    {
        return std::nullopt;
    }
    if (header->magic != FS_DAEMON_MAGIC)
    {
        return std::nullopt;
    }
    if (header->version < FS_DAEMON_PROTOCOL_MIN || header->version > FS_DAEMON_PROTOCOL_MAX)
    {
        return std::nullopt;
    }
    return header;
}

// Encode @text into @field, always leaving the last byte a NUL: a value that would fill or overflow
// the field is cut so nothing downstream reads past a terminator. The field's own width comes with
// it, so no caller can hand a name the length constant of a different field.
template <std::uint64_t width>
void writeName(char (&field)[width], std::string_view text) noexcept
{
    const auto kept = std::min<std::uint64_t>(text.size(), width - 1);
    std::memcpy(static_cast<char*>(field), text.data(), kept);
    std::memset(static_cast<char*>(field) + kept, 0, width - kept);
}

// The string @field holds, up to its first NUL. A field with no NUL is read whole.
template <std::uint64_t width>
[[nodiscard]] std::string readName(const char (&field)[width])
{
    const auto* stop = static_cast<const char*>(std::memchr(field, '\0', width));
    const auto length = (stop == nullptr) ? width : static_cast<std::uint64_t>(stop - field);
    return std::string{static_cast<const char*>(field), length};
}

// One whole frame: the header for @type, @seq and the length of @payload, then @payload itself. Both
// the daemon's writer and a probe's put a frame out in one write, so both assemble it here.
[[nodiscard]] inline std::vector<std::byte> makeFrame(std::uint32_t type, std::uint64_t seq,
                                                      ByteView payload)
{
    const auto header = makeHeader(type, seq, static_cast<std::uint32_t>(payload.size()));
    const auto headerBytes = encode(header);

    std::vector<std::byte> frame;
    frame.reserve(HeaderBytes + payload.size());
    frame.insert(frame.end(), headerBytes.begin(), headerBytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

}  // namespace fsdaemon::wire
