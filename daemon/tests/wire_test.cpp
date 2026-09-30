// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/wire_test.cpp -- the on-wire encoding.
//
// The struct sizes, the field offsets, the NUL handling, the domain truncation and the signed
// status are the ABI the kernel reads, so each is checked rather than trusted.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "harness/probe.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::FsDaemonAccessRequest;
using fsdaemon::FsDaemonAttestRequest;
using fsdaemon::FsDaemonAttestResponse;
using fsdaemon::FsDaemonHdr;
using fsdaemon::FsDaemonLockRequest;
using fsdaemon::FsDaemonLockResponse;
using fsdaemon::FsDaemonResponse;
using fsdaemon::FsDaemonUnlockRequest;
using fsdaemon::wire::decode;
using fsdaemon::wire::encode;
using fsdaemon::wire::makeHeader;
using fsdaemon::wire::parseHeader;
using fsdaemon::wire::readName;
using fsdaemon::wire::resolvePermBit;
using fsdaemon::wire::writeName;

fsdaemon::wire::ByteView bytesOf(const void* data, std::uint64_t size)
{
    return {static_cast<const std::byte*>(data), size};
}

void checkHeaderRoundtrip(fsdaemon::probe::Context& ctx)
{
    const FsDaemonHdr header = makeHeader(FS_DAEMON_MSG_ACCESS_REQUEST, 42, 40);
    const auto raw = encode(header);
    ctx.checkf(raw.size() == 32, "header encodes to %zu bytes (want 32)", raw.size());

    const auto parsed = parseHeader(bytesOf(raw.data(), raw.size()));
    ctx.check(parsed.has_value(), "a well-formed header parses");
    if (parsed)
    {
        ctx.check(parsed->version == FS_DAEMON_PROTOCOL_VERSION, "version is the protocol version");
        ctx.check(parsed->type == FS_DAEMON_MSG_ACCESS_REQUEST, "type round-trips");
        ctx.checkf(parsed->seq == 42, "seq round-trips (%llu)", static_cast<unsigned long long>(parsed->seq));
        ctx.check(parsed->payload_len == 40, "payload_len round-trips");
    }
}

void refuseBadMagicHeader(fsdaemon::probe::Context& ctx)
{
    FsDaemonHdr header = makeHeader(FS_DAEMON_MSG_ATTEST_REQUEST, 1, 0);
    header.magic = 0xdeadbeef;
    const auto raw = encode(header);
    ctx.check(!parseHeader(bytesOf(raw.data(), raw.size())).has_value(), "a wrong magic is refused");

    header.magic = FS_DAEMON_MAGIC;
    header.version = FS_DAEMON_PROTOCOL_MAX + 1;
    const auto rawVer = encode(header);
    ctx.check(!parseHeader(bytesOf(rawVer.data(), rawVer.size())).has_value(),
              "a version past the window is refused");
}

void checkAccessRequestRoundtrip(fsdaemon::probe::Context& ctx)
{
    FsDaemonAccessRequest request{};
    request.consumer_pid = 12345;
    request.consumer_uid = 1004;
    request.consumer_gid = 100;
    request.consumer_start_boottime_ns = 0;
    request.consumer_exe_inode_ino = 10001;
    request.consumer_exe_inode_dev = 5;
    writeName(request.owner_group, "prod");
    writeName(request.owner_role, "llm-worker");
    writeName(request.consumer_exe_path, "/usr/local/bin/vllm");

    const auto raw = encode(request);
    ctx.checkf(raw.size() == 256, "access_request is %zu bytes (want 256)", raw.size());

    const auto decoded = decode<FsDaemonAccessRequest>(bytesOf(raw.data(), raw.size()));
    ctx.check(decoded.has_value(), "access_request decodes");
    if (decoded)
    {
        ctx.check(decoded->consumer_pid == 12345, "consumer_pid round-trips");
        ctx.check(decoded->consumer_uid == 1004, "consumer_uid round-trips");
        ctx.check(decoded->consumer_gid == 100, "consumer_gid round-trips");
        ctx.check(decoded->consumer_exe_inode_ino == 10001, "consumer_exe_inode_ino round-trips");
        ctx.check(decoded->consumer_exe_inode_dev == 5, "consumer_exe_inode_dev round-trips");
        ctx.check(readName(decoded->owner_group) == "prod", "owner_group trims to prod");
        ctx.check(readName(decoded->owner_role) == "llm-worker", "owner_role trims");
        ctx.check(readName(decoded->consumer_exe_path) == "/usr/local/bin/vllm",
                  "consumer_exe_path trims");
    }
}

void checkAccessResponsePack(fsdaemon::probe::Context& ctx)
{
    FsDaemonResponse response{};
    response.status = 0;
    response.granted_perms = FS_PERM_READ | FS_PERM_WRITE;
    response.expiry_secs = 3600;
    const auto raw = encode(response);
    ctx.checkf(raw.size() == 24, "access response is %zu bytes (want 24)", raw.size());
}

void checkAttestRequestUnpack(fsdaemon::probe::Context& ctx)
{
    FsDaemonAttestRequest request{};
    request.owner_pid = 777;
    request.owner_uid = 1004;
    request.owner_gid = 100;
    request.owner_start_boottime_ns = 424242;
    request.owner_exe_inode_ino = 20002;
    request.owner_exe_inode_dev = 6;
    writeName(request.owner_exe_path, "/usr/bin/prefiller");

    const auto raw = encode(request);
    ctx.checkf(raw.size() == 224, "attest_request is %zu bytes (want 224)", raw.size());

    const auto decoded = decode<FsDaemonAttestRequest>(bytesOf(raw.data(), raw.size()));
    ctx.check(decoded.has_value(), "attest_request decodes");
    if (decoded)
    {
        ctx.check(decoded->owner_pid == 777, "owner_pid round-trips");
        ctx.check(decoded->owner_start_boottime_ns == 424242, "owner_start_boottime_ns round-trips");
        ctx.check(readName(decoded->owner_exe_path) == "/usr/bin/prefiller",
                  "owner_exe_path trims");
    }
}

void checkAttestResponseOffsets(fsdaemon::probe::Context& ctx)
{
    FsDaemonAttestResponse response{};
    response.status = 0;
    writeName(response.group, "prod");
    writeName(response.role, "llm-worker");
    const auto raw = encode(response);
    ctx.checkf(raw.size() == 40, "attest_response is %zu bytes (want 40)", raw.size());

    // The group field starts at offset 8 and the role at 24; those offsets are the ABI.
    ctx.check(std::memcmp(raw.data() + 8, "prod\0", 5) == 0, "group sits at offset 8");
    ctx.check(std::memcmp(raw.data() + 24, "llm-worker\0", 11) == 0, "role sits at offset 24");
}

void checkPermBitFromName(fsdaemon::probe::Context& ctx)
{
    ctx.check(resolvePermBit("READ") == FS_PERM_READ, "READ resolves to 0x01");
    ctx.check(resolvePermBit("WRITE") == FS_PERM_WRITE, "WRITE resolves to 0x02");
    ctx.check(resolvePermBit("ADMIN") == FS_PERM_ADMIN, "ADMIN resolves to 0x08");
    ctx.check(resolvePermBit("ALL") == FS_PERM_ALL, "ALL resolves to 0x3F");
    ctx.check(!resolvePermBit("BOGUS").has_value(), "an unknown perm name is refused");
    ctx.check(!resolvePermBit("read").has_value(), "a lower-case perm name is refused");
}

void checkLockRoundtrip(fsdaemon::probe::Context& ctx)
{
    FsDaemonLockRequest request{};
    writeName(request.domain, FS_DOMAIN_META);
    request.timeout_ms = 1500;
    const auto raw = encode(request);
    ctx.checkf(raw.size() == 24, "lock_request is %zu bytes (want 24)", raw.size());

    const auto decoded = decode<FsDaemonLockRequest>(bytesOf(raw.data(), raw.size()));
    ctx.check(decoded.has_value(), "lock_request decodes");
    if (decoded)
    {
        ctx.check(readName(decoded->domain) == FS_DOMAIN_META, "domain round-trips");
        ctx.check(decoded->timeout_ms == 1500, "timeout_ms round-trips");
    }
}

void checkLockDomainTerminator(fsdaemon::probe::Context& ctx)
{
    FsDaemonLockRequest request{};
    const std::string overlong(40, 'd');
    writeName(request.domain, overlong);
    ctx.check(request.domain[FS_DAEMON_DOMAIN_LEN - 1] == '\0', "an overlong domain keeps a terminator");
    const std::string readBack = readName(request.domain);
    ctx.checkf(readBack.size() == FS_DAEMON_DOMAIN_LEN - 1, "an overlong domain reads back cut to %d",
               FS_DAEMON_DOMAIN_LEN - 1);
}

void checkUnlockRoundtrip(fsdaemon::probe::Context& ctx)
{
    FsDaemonUnlockRequest request{};
    writeName(request.domain, FS_DOMAIN_META);
    const auto raw = encode(request);
    ctx.checkf(raw.size() == 16, "unlock_request is %zu bytes (want 16)", raw.size());
    const auto decoded = decode<FsDaemonUnlockRequest>(bytesOf(raw.data(), raw.size()));
    ctx.check(decoded && readName(decoded->domain) == FS_DOMAIN_META,
              "unlock domain round-trips");
}

void checkLockResponseSignedStatus(fsdaemon::probe::Context& ctx)
{
    FsDaemonLockResponse response{};
    response.status = -EBUSY;
    const auto raw = encode(response);
    ctx.checkf(raw.size() == 8, "lock_response is %zu bytes (want 8)", raw.size());
    const auto decoded = decode<FsDaemonLockResponse>(bytesOf(raw.data(), raw.size()));
    ctx.check(decoded && decoded->status == -EBUSY, "a negative status round-trips signed");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkHeaderRoundtrip(ctx);
            refuseBadMagicHeader(ctx);
            checkAccessRequestRoundtrip(ctx);
            checkAccessResponsePack(ctx);
            checkAttestRequestUnpack(ctx);
            checkAttestResponseOffsets(ctx);
            checkPermBitFromName(ctx);
            checkLockRoundtrip(ctx);
            checkLockDomainTerminator(ctx);
            checkUnlockRoundtrip(ctx);
            checkLockResponseSignedStatus(ctx);
        });
}
