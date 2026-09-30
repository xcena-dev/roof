// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/opa_test.cpp -- the OPA REST backend over a canned transport.
//
// The transport is a seam, so this drives the whole decide path with a canned server body and never
// opens a socket. It checks url validation, the input envelope, the allow/deny/fail-closed matrix,
// and that an unreachable server or an unreadable body is a denial.

#include "policy/internal/opa.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#include "harness/probe.hpp"
#include "model.hpp"
#include "policy/base.hpp"
#include "util/text.hpp"
#include "wire/protocol.hpp"

namespace
{

using fsdaemon::policy::Decision_t;
using fsdaemon::policy::OpaPolicyEngine;
using fsdaemon::policy::OpaTransport;
using fsdaemon::policy::OpaTransportError;

fsdaemon::Identity_t consumerFixture()
{
    fsdaemon::Identity_t who;
    who.spiffeId = "spiffe://test.local/group/eng/role/reader";
    who.group = "eng";
    who.role = "reader";
    return who;
}

// A transport that ignores its inputs and always answers @body.
OpaTransport constantBody(const std::string& body)
{
    return [body](const std::string&, const std::string&)
    {
        return body;
    };
}

void refuseNonHttpUrl(fsdaemon::probe::Context& ctx)
{
    bool threw = false;
    try
    {
        const OpaPolicyEngine engine{"ftp://opa.local/v1/data/authz", constantBody("{}")};
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    ctx.check(threw, "a non-http(s) url is refused at construction");

    bool acceptedHttp = true;
    try
    {
        const OpaPolicyEngine http{"http://opa.local/v1/data/authz", constantBody("{}")};
        const OpaPolicyEngine https{"https://opa.local/v1/data/authz", constantBody("{}")};
    }
    catch (const std::invalid_argument&)
    {
        acceptedHttp = false;
    }
    ctx.check(acceptedHttp, "http and https urls are accepted");
}

void checkInputEnvelopeBuild(fsdaemon::probe::Context& ctx)
{
    std::string sent;
    OpaPolicyEngine engine{"http://opa.local/v1/data/authz",
                           [&sent](const std::string&, const std::string& requestBody)
                           {
                               sent = requestBody;
                               return std::string{"{}"};
                           }};
    static_cast<void>(engine.decide(consumerFixture(), "eng", "owner"));
    ctx.check(fsdaemon::util::hasPrefix(sent, R"({"input":)"), "the request wraps the input in an input member");
    ctx.check(sent.find(R"("spiffe_id":"spiffe://test.local/group/eng/role/reader")") != std::string::npos,
              "the consumer spiffe id is carried");
    ctx.check(sent.find(R"("owner":{"group":"eng","role":"owner"})") != std::string::npos,
              "the owner group and role are carried");
}

void checkAllowMapsToPerms(fsdaemon::probe::Context& ctx)
{
    OpaPolicyEngine engine{
        "http://opa.local/v1/data/authz",
        constantBody(R"({"result":{"allow":true,"granted_perms":["READ","WRITE"],"rule":"r1"}})")};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");
    ctx.check(decision.allow, "an allow result is an allow");
    ctx.check(decision.grantedPerms == (FS_PERM_READ | FS_PERM_WRITE), "the perm names become the bits");
    ctx.check(decision.rule == "r1", "the rule name is carried");
}

void checkDenyMapping(fsdaemon::probe::Context& ctx)
{
    OpaPolicyEngine engine{"http://opa.local/v1/data/authz",
                           constantBody(R"({"result":{"allow":false,"rule":"r2"}})")};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");
    ctx.check(!decision.allow, "a deny result is a deny");
    ctx.check(decision.grantedPerms == 0, "a deny grants nothing");
}

void checkFailClosedOnUnreachableServer(fsdaemon::probe::Context& ctx)
{
    const OpaTransport down = [](const std::string&, const std::string&) -> std::string
    {
        throw OpaTransportError{"connection refused"};
    };
    OpaPolicyEngine engine{"http://opa.local/v1/data/authz", down};
    const Decision_t decision = engine.decide(consumerFixture(), "eng", "owner");
    ctx.check(!decision.allow, "an unreachable server is a denial, not a throw");
}

void checkFailClosedOnUnreadableBody(fsdaemon::probe::Context& ctx)
{
    OpaPolicyEngine notJson{"http://opa.local/v1/data/authz", constantBody("this is not json")};
    ctx.check(!notJson.decide(consumerFixture(), "eng", "owner").allow, "a non-json body is a denial");

    OpaPolicyEngine noResult{"http://opa.local/v1/data/authz", constantBody("{\"other\":1}")};
    ctx.check(!noResult.decide(consumerFixture(), "eng", "owner").allow,
              "a body with no result is a denial");

    OpaPolicyEngine badPerm{
        "http://opa.local/v1/data/authz",
        constantBody(R"({"result":{"allow":true,"granted_perms":["FLY"]}})")};
    ctx.check(!badPerm.decide(consumerFixture(), "eng", "owner").allow,
              "an unknown perm name is a denial");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            refuseNonHttpUrl(ctx);
            checkInputEnvelopeBuild(ctx);
            checkAllowMapsToPerms(ctx);
            checkDenyMapping(ctx);
            checkFailClosedOnUnreachableServer(ctx);
            checkFailClosedOnUnreadableBody(ctx);
        });
}
