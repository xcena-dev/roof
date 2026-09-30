// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/access_test.cpp -- the ACCESS path end to end: attest the consumer, then decide the policy.
//
// The daemon serves over a socketpair with a real identity backend and a real rego policy. An access
// for a consumer whose group matches the owner is granted with perms; one whose group does not is
// denied. The real channel is never opened.

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "harness/probe.hpp"
#include "harness/socket_pair.hpp"
#include "harness/temp_file.hpp"
#include "harness/wire_io.hpp"
#include "identity/internal/local.hpp"
#include "name.hpp"
#include "policy/internal/local.hpp"
#include "posix/unique_fd.hpp"
#include "serve/channel.hpp"
#include "serve/handler.hpp"
#include "serve/loop.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::FsDaemonAccessRequest;
using fsdaemon::FsDaemonResponse;
using fsdaemon::probe::readResponse;
using fsdaemon::probe::SocketPair;
using fsdaemon::probe::TempFile;
using fsdaemon::probe::writeFrame;
using fsdaemon::serve::FsChannel;
using fsdaemon::serve::RequestHandler;
using fsdaemon::serve::ServeLoop;
using fsdaemon::serve::Services_t;
using fsdaemon::wire::writeName;

const std::string PolicyRego = std::string{"package "} + std::string{fsdaemon::name::FsName} + R"(.authz

import future.keywords.if

default allow := false

allow if {
    input.consumer.group == "prod"
    input.owner.group == input.consumer.group
}

granted_perms := ["READ", "WRITE"] if allow
expiry_secs := 3600 if allow
rule := "same-group" if allow
)";

void sendAccess(fsdaemon::posix::Descriptor descriptor, std::uint64_t seq, const char* ownerGroup,
                const char* ownerRole)
{
    FsDaemonAccessRequest request{};
    request.consumer_pid = static_cast<std::uint32_t>(::getpid());
    request.consumer_uid = ::getuid();
    request.consumer_gid = ::getgid();
    request.consumer_start_boottime_ns = 0;
    writeName(request.owner_group, ownerGroup);
    writeName(request.owner_role, ownerRole);
    writeFrame(descriptor, FS_DAEMON_MSG_ACCESS_REQUEST, seq, request);
}

void checkAllowAndDeny(fsdaemon::probe::Context& ctx)
{
    SocketPair pair;
    if (!ctx.check(pair.isOpen(), "a socketpair stands in for the channel"))
    {
        return;
    }

    const TempFile rules{
        "rules:\n"
        "  - match:\n"
        "      uid: " +
            std::to_string(::getuid()) +
            "\n"
            "    identity:\n"
            "      group: prod\n"
            "      role: llm-worker\n",
        "daemon-access-rules"};
    const TempFile policy{PolicyRego, "daemon-access-policy"};

    fsdaemon::identity::LocalIdentityProvider identity{rules.getPath(), std::vector<std::string>{}};
    fsdaemon::policy::LocalPolicyEngine engine{policy.getPath(),
                                               fsdaemon::policy::LocalPolicyEngine::getDefaultQuery()};
    Services_t services{identity};
    services.policy = &engine;
    RequestHandler handler{services};
    auto channel = FsChannel::makeChannel(pair.releaseTheirs());
    ServeLoop loop{channel, handler};
    std::thread runner{[&loop]
                       {
                           loop.run();
                       }};

    // Owner in the same group: granted with perms.
    sendAccess(pair.getMine(), 31, "prod", "llm-worker");
    const auto allow = readResponse<FsDaemonResponse>(pair.getMine(), FS_DAEMON_MSG_ACCESS_RESPONSE);
    ctx.check(allow.has_value() && allow->payload.status == 0, "a same-group access is granted");
    ctx.checkf(allow && allow->payload.granted_perms == 0x03U, "it grants READ|WRITE (0x%x)",
               allow ? allow->payload.granted_perms : 0);
    ctx.check(allow && allow->payload.expiry_secs == 3600, "it carries the policy expiry");

    // Owner in another group: denied.
    sendAccess(pair.getMine(), 32, "other", "role");
    const auto deny = readResponse<FsDaemonResponse>(pair.getMine(), FS_DAEMON_MSG_ACCESS_RESPONSE);
    ctx.check(deny.has_value() && deny->payload.status == -EACCES, "a cross-group access is denied");

    loop.stop();
    runner.join();
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe([](fsdaemon::probe::Context& ctx)
                                     {
                                         checkAllowAndDeny(ctx);
                                     });
}
