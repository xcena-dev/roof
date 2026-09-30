// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/channel.hpp -- the channel to the kernel: /dev/<daemon>-<node>, framed.
//
// The kernel sends one request frame at a time and matches a response to it by seq, so the two
// directions are independent. Reads take whatever is ready as whole frames; writes put one whole
// frame out under a lock, because several worker threads answer at once and a frame must not
// interleave with another on the wire.
//
// A byte stream is framed by length rather than by read boundary, so a socketpair stand-in and a
// datagram channel both extract the same frames: the test never opens the real channel.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "posix/unique_fd.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace fsdaemon::observe
{
class Stat;
}

namespace fsdaemon::serve
{

// One request off the wire: its header and a copy of its payload bytes. The payload is a copy, not
// a view, so a frame outlives the read buffer it came from.
struct Frame_t
{
    FsDaemonHdr header{};
    std::vector<std::byte> payload;
};

class FsChannel
{
public:
    // ── factories ──────────────────────────────────────────────────
    // Opens the char device O_RDWR, non-blocking and close-on-exec. The driver's .open never sees
    // O_CLOEXEC, since the VFS sets it on the descriptor instead. Throws std::system_error on failure.
    [[nodiscard]] static FsChannel makeChannel(const std::string& path);

    // Takes an already-open descriptor and closes it with the FsChannel, which is how a test hands in
    // one end of a socketpair. Sets it non-blocking, so a read takes what is ready and stops.
    [[nodiscard]] static FsChannel makeChannel(posix::Descriptor descriptor);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] posix::Descriptor getDescriptor() const noexcept
    {
        return held_.get();
    }

    // Records this channel's stages into @stat, or nothing when null. Set after construction
    // because the factories take a path or a descriptor and nothing else.
    void observe(::fsdaemon::observe::Stat* stat) noexcept
    {
        stat_ = stat;
    }

    // ── reading ────────────────────────────────────────────────────
    // Reads everything ready without blocking and returns the whole frames in it; a partial frame
    // stays buffered. A bad header or an over-cap payload_len clears the buffer, since a byte stream
    // cannot be resynced.
    [[nodiscard]] std::vector<Frame_t> readReady();

    // Whether the peer has hung up. Sticky: a killed kernel side or a closed test socket does not
    // come back, so a caller that sees this stops rather than reading again.
    [[nodiscard]] bool isEndOfStream() const noexcept
    {
        return endOfStream_;
    }

    // ── writing ────────────────────────────────────────────────────
    // The HELLO the helper sends first, announcing what it will answer.
    bool sendHello(std::uint64_t capabilities, std::uint32_t helperPid);

    bool sendAttestResponse(std::uint64_t seq, std::int32_t status,
                            std::string_view group, std::string_view role);
    bool sendAccessResponse(std::uint64_t seq, std::int32_t status,
                            std::uint32_t grantedPerms, std::int64_t expirySecs);
    bool sendLockResponse(std::uint64_t seq, std::uint32_t type, std::int32_t status);

private:
    // Built only by the factories, which return a prvalue so the mutex member needs no move.
    explicit FsChannel(posix::UniqueFd held) noexcept
        : held_{std::move(held)}
    {
    }

    // One whole frame out, header then payload, under the write lock. False when the descriptor
    // failed in a way a retry would not fix.
    bool sendFrame(std::uint32_t type, std::uint64_t seq, wire::ByteView payload);

    // ── the descriptor this FsChannel closes ──────────────────────────
    posix::UniqueFd held_;

    // ── what a read is mid-way through ─────────────────────────────
    // Bytes read but not yet a whole frame. Only the reader thread touches it.
    std::vector<std::byte> pending_;

    bool endOfStream_{false};

    ::fsdaemon::observe::Stat* stat_{nullptr};

    // ── one writer at a time ───────────────────────────────────────
    std::mutex writing_;
};

}  // namespace fsdaemon::serve
