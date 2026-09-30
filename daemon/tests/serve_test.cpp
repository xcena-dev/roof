// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/serve_test.cpp -- the serve loop end to end, over a socketpair standing in for the channel.
//
// This is the attest milestone as the kernel would see it: a request frame in, a response frame out,
// on a running loop. It also checks the two fail-closed answers the handler gives without a policy
// engine and without a turn service.

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "harness/probe.hpp"
#include "harness/socket_pair.hpp"
#include "harness/temp_file.hpp"
#include "harness/wire_io.hpp"
#include "identity/internal/local.hpp"
#include "serve/channel.hpp"
#include "serve/handler.hpp"
#include "serve/loop.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::FsDaemonAccessRequest;
using fsdaemon::FsDaemonAttestRequest;
using fsdaemon::FsDaemonAttestResponse;
using fsdaemon::FsDaemonLockRequest;
using fsdaemon::FsDaemonLockResponse;
using fsdaemon::FsDaemonResponse;
using fsdaemon::probe::readResponse;
using fsdaemon::probe::SocketPair;
using fsdaemon::probe::TempFile;
using fsdaemon::probe::writeFrame;
using fsdaemon::serve::FsChannel;
using fsdaemon::serve::RequestHandler;
using fsdaemon::serve::ServeLoop;
using fsdaemon::serve::Services_t;
using fsdaemon::wire::readName;
using fsdaemon::wire::writeName;

// A rules file naming this process, so the attest below resolves against its own uid.
std::string selfRule()
{
    return "rules:\n  - match:\n      uid: " + std::to_string(::getuid()) +
           "\n    identity:\n      group: prod\n      role: llm-worker\n";
}

void checkAttestOverLoop(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }

    const TempFile rules{selfRule(), "daemon-serve-rules"};
    fsdaemon::identity::LocalIdentityProvider identity{rules.getPath(), {}};
    RequestHandler handler{Services_t{identity}};
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());
    ServeLoop loop{channel, handler};
    std::thread runner{[&loop]
                       {
                           loop.run();
                       }};

    FsDaemonAttestRequest request{};
    request.owner_pid = static_cast<std::uint32_t>(::getpid());
    request.owner_uid = ::getuid();
    request.owner_gid = ::getgid();
    writeFrame(pair.getMine(), FS_DAEMON_MSG_ATTEST_REQUEST, 21, request);

    const auto attest =
        readResponse<FsDaemonAttestResponse>(pair.getMine(), FS_DAEMON_MSG_ATTEST_RESPONSE);
    if (ctx.check(attest.has_value(), "an attest gets an attest response") && attest)
    {
        ctx.check(attest->seq == 21, "the response seq matches the request");
        ctx.check(attest->payload.status == 0, "the attest succeeds");
        ctx.check(readName(attest->payload.group) == "prod",
                  "the resolved group comes back");
        ctx.check(readName(attest->payload.role) == "llm-worker",
                  "the resolved role comes back");
    }

    FsDaemonAccessRequest access{};
    access.consumer_pid = static_cast<std::uint32_t>(::getpid());
    access.consumer_uid = ::getuid();
    writeFrame(pair.getMine(), FS_DAEMON_MSG_ACCESS_REQUEST, 22, access);
    const auto denied = readResponse<FsDaemonResponse>(pair.getMine(), FS_DAEMON_MSG_ACCESS_RESPONSE);
    ctx.check(denied && denied->payload.status == -EACCES, "access is denied fail-closed");

    FsDaemonLockRequest lock{};
    writeName(lock.domain, FS_DOMAIN_META);
    writeFrame(pair.getMine(), FS_DAEMON_MSG_LOCK_REQUEST, 23, lock);
    const auto noTurn = readResponse<FsDaemonLockResponse>(pair.getMine(), FS_DAEMON_MSG_LOCK_RESPONSE);
    ctx.check(noTurn && noTurn->payload.status == -EOPNOTSUPP, "a lock reports no turn service");

    loop.stop();
    runner.join();
}

// One attest through the loop against @rulesText, answering the response's status.
std::optional<std::int32_t> attestStatusUnder(fsdaemon::probe::Context& ctx, const std::string& rulesText,
                                              std::uint64_t seq)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return std::nullopt;
    }

    const TempFile rules{rulesText, "daemon-serve-rules"};
    fsdaemon::identity::LocalIdentityProvider identity{rules.getPath(), {}};
    RequestHandler handler{Services_t{identity}};
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());
    ServeLoop loop{channel, handler};
    std::thread runner{[&loop]
                       {
                           loop.run();
                       }};

    FsDaemonAttestRequest request{};
    request.owner_pid = static_cast<std::uint32_t>(::getpid());
    request.owner_uid = ::getuid();
    request.owner_gid = ::getgid();
    writeFrame(pair.getMine(), FS_DAEMON_MSG_ATTEST_REQUEST, seq, request);
    const auto attest =
        readResponse<FsDaemonAttestResponse>(pair.getMine(), FS_DAEMON_MSG_ATTEST_RESPONSE);

    loop.stop();
    runner.join();
    if (!attest)
    {
        return std::nullopt;
    }
    return attest->payload.status;
}

// The wire holds FS_DAEMON_IDENT_LEN - 1 bytes of a label. A label the rules resolve to that would
// not fit is refused whole, since a cut one would reach the kernel as another tenant's prefix. An
// empty one names nobody and is refused the same way.
void refuseUnwireableLabel(fsdaemon::probe::Context& ctx)
{
    const std::string longGroup(FS_DAEMON_IDENT_LEN, 'g');
    const auto overlong = attestStatusUnder(ctx,
                                            "rules:\n  - match:\n      uid: " + std::to_string(::getuid()) +
                                                "\n    identity:\n      group: " + longGroup +
                                                "\n      role: llm-worker\n",
                                            31);
    ctx.check(overlong.has_value() && *overlong == -EACCES,
              "a group one byte longer than the wire field is refused rather than cut");

    const auto fitting = attestStatusUnder(ctx,
                                           "rules:\n  - match:\n      uid: " + std::to_string(::getuid()) +
                                               "\n    identity:\n      group: " +
                                               std::string(FS_DAEMON_IDENT_LEN - 1, 'g') + "\n      role: llm-worker\n",
                                           32);
    ctx.check(fitting.has_value() && *fitting == 0, "a group that just fits the wire field reads through");

    const auto empty = attestStatusUnder(ctx,
                                         "rules:\n  - match:\n      uid: " + std::to_string(::getuid()) +
                                             "\n    identity:\n      group: prod\n      role: \"\"\n",
                                         33);
    ctx.check(empty.has_value() && *empty == -EACCES, "an empty role is refused rather than sent as nobody");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkAttestOverLoop(ctx);
            refuseUnwireableLabel(ctx);
        });
}
