// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/channel_test.cpp -- the channel framing.
//
// A socketpair stands in for the kernel char device: the real /dev/<daemon>-* is never opened. The
// test writes whole request frames into one end and reads responses back, and checks that a frame
// split across reads still assembles and a burst of frames all come out.

#include "serve/channel.hpp"

#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/probe.hpp"
#include "harness/socket_pair.hpp"
#include "harness/temp_file.hpp"
#include "harness/wire_io.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::FsDaemonAccessRequest;
using fsdaemon::FsDaemonHdr;
using fsdaemon::FsDaemonLockRequest;
using fsdaemon::FsDaemonResponse;
using fsdaemon::FsDaemonUnlockRequest;
using fsdaemon::probe::readExactly;
using fsdaemon::probe::SocketPair;
using fsdaemon::probe::TempFile;
using fsdaemon::probe::writeAll;
using fsdaemon::serve::FsChannel;
using fsdaemon::wire::decode;
using fsdaemon::wire::encode;
using fsdaemon::wire::HeaderBytes;
using fsdaemon::wire::makeHeader;
using fsdaemon::wire::parseHeader;
using fsdaemon::wire::readName;
using fsdaemon::wire::writeName;

// One frame's bytes: header then payload, as the kernel would put it on the wire.
std::vector<std::byte> makeFrame(std::uint32_t type, std::uint64_t seq, fsdaemon::wire::ByteView payload)
{
    const FsDaemonHdr header = makeHeader(type, seq, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::byte> frame(HeaderBytes + payload.size());
    std::memcpy(frame.data(), &header, HeaderBytes);
    std::memcpy(frame.data() + HeaderBytes, payload.data(), payload.size());
    return frame;
}

std::vector<std::byte> accessRequestPayload()
{
    FsDaemonAccessRequest request{};
    request.consumer_pid = 4242;
    request.consumer_uid = 1004;
    request.consumer_gid = 100;
    request.consumer_start_boottime_ns = 0;
    request.consumer_exe_inode_ino = 10001;
    request.consumer_exe_inode_dev = 5;
    writeName(request.owner_group, "prod");
    writeName(request.owner_role, "llm-worker");
    writeName(request.consumer_exe_path, "/usr/local/bin/vllm");
    const auto bytes = encode(request);
    return {bytes.begin(), bytes.end()};
}

void checkRequestResponse(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());

    const std::vector<std::byte> payload = accessRequestPayload();
    const std::vector<std::byte> frame = makeFrame(FS_DAEMON_MSG_ACCESS_REQUEST, 11, payload);
    writeAll(pair.getMine(), frame);

    const auto ready = channel.readReady();
    ctx.checkf(ready.size() == 1, "one request arrives (%zu)", ready.size());
    if (ready.size() == 1)
    {
        const auto& one = ready.front();
        ctx.check(one.header.type == FS_DAEMON_MSG_ACCESS_REQUEST, "type is ACCESS_REQUEST");
        ctx.check(one.header.seq == 11, "seq survives framing");
        const auto request = decode<FsDaemonAccessRequest>(one.payload);
        ctx.check(request && request->consumer_pid == 4242, "payload decodes to the consumer pid");
        ctx.check(request && readName(request->owner_group) == "prod",
                  "payload carries owner_group");
    }

    ctx.check(channel.sendAccessResponse(11, 0, FS_PERM_READ | FS_PERM_WRITE, 3600),
              "a response writes");
    const std::vector<std::byte> back =
        readExactly(pair.getMine(), HeaderBytes + sizeof(FsDaemonResponse));
    ctx.checkf(back.size() == HeaderBytes + sizeof(FsDaemonResponse),
               "the response frame is header+24 (%zu)", back.size());
    const auto header = parseHeader(back);
    ctx.check(header.has_value(), "the response header parses");
    if (header)
    {
        ctx.check(header->type == FS_DAEMON_MSG_ACCESS_RESPONSE, "response type is ACCESS_RESPONSE");
        ctx.check(header->seq == 11, "response seq matches the request");
        ctx.check(header->payload_len == sizeof(FsDaemonResponse), "response payload_len is 24");
    }
}

void checkFrameReassembly(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());

    const std::vector<std::byte> payload = accessRequestPayload();
    const std::vector<std::byte> frame = makeFrame(FS_DAEMON_MSG_ACCESS_REQUEST, 7, payload);

    // The header and a first slice of the payload, but not the whole frame.
    const auto split = HeaderBytes + 8;
    writeAll(pair.getMine(), fsdaemon::wire::ByteView{frame.data(), split});
    const auto first = channel.readReady();
    ctx.check(first.empty(), "a partial frame yields nothing yet");

    writeAll(pair.getMine(), fsdaemon::wire::ByteView{frame.data() + split, frame.size() - split});
    const auto second = channel.readReady();
    ctx.check(second.size() == 1, "the rest of the frame completes it");
}

void checkFrameBurst(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());

    FsDaemonLockRequest lockReq{};
    writeName(lockReq.domain, FS_DOMAIN_META);
    lockReq.timeout_ms = 250;
    const auto lockBytes = encode(lockReq);
    FsDaemonUnlockRequest unlockReq{};
    writeName(unlockReq.domain, FS_DOMAIN_META);
    const auto unlockBytes = encode(unlockReq);

    std::vector<std::byte> both;
    const std::vector<std::byte> lockFrame = makeFrame(FS_DAEMON_MSG_LOCK_REQUEST, 5, lockBytes);
    const std::vector<std::byte> unlockFrame = makeFrame(FS_DAEMON_MSG_UNLOCK_REQUEST, 6, unlockBytes);
    both.insert(both.end(), lockFrame.begin(), lockFrame.end());
    both.insert(both.end(), unlockFrame.begin(), unlockFrame.end());
    writeAll(pair.getMine(), both);

    const auto ready = channel.readReady();
    ctx.checkf(ready.size() == 2, "a burst of two frames both arrive (%zu)", ready.size());
    if (ready.size() == 2)
    {
        const auto lock = decode<FsDaemonLockRequest>(ready[0].payload);
        ctx.check(ready[0].header.type == FS_DAEMON_MSG_LOCK_REQUEST && lock &&
                      lock->timeout_ms == 250,
                  "the lock frame parses with its timeout");
        ctx.check(ready[1].header.type == FS_DAEMON_MSG_UNLOCK_REQUEST &&
                      ready[1].header.seq == 6,
                  "the unlock frame parses with its seq");
    }
}

void checkPathSetsCloexec(fsdaemon::probe::Context& ctx)
{
    // A regular temp file, never the real channel: the check is only that the open path sets the
    // close-on-exec bit the kernel strips before it calls .open.
    const TempFile file{"", "daemon-cloexec"};
    auto channel = FsChannel::makeChannel(file.getPath());
    const auto flags = ::fcntl(channel.getDescriptor(), F_GETFD);
    ctx.check(flags >= 0 && (flags & FD_CLOEXEC) != 0, "makeChannel sets FD_CLOEXEC");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkRequestResponse(ctx);
            checkFrameReassembly(ctx);
            checkFrameBurst(ctx);
            checkPathSetsCloexec(ctx);
        });
}
