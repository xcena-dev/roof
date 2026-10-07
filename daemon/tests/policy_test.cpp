// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/policy_test.cpp -- the in-process rego policy backend, and the fail-closed reading of a
// decision.
//
// It writes a rego policy to a temp file, evaluates real requests through regorus, and checks the
// decision reading denies on every malformed result.

#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "model.hpp"
#include "name.hpp"
#include "policy/base.hpp"
#include "policy/internal/local.hpp"

namespace
{

using fsdaemon::Identity_t;
using fsdaemon::policy::Decision_t;
using fsdaemon::policy::LocalPolicyEngine;
using fsdaemon::policy::readDecision;
using fsdaemon::probe::TempFile;

// A policy with a same-group rule and an admin rule, the two the cases below assert over.
const std::string PolicyRego = std::string{"package "} + std::string{fsdaemon::name::FsName} + R"(.authz

import future.keywords.if

default allow := false

same_group if {
    input.consumer.group == "prod"
    input.consumer.role == "llm-worker"
    input.owner.group == input.consumer.group
}

admin if {
    input.consumer.role == "cache-admin"
}

allow if same_group
allow if admin

rule := "same-group" if { same_group; not admin }
rule := "admin" if admin

granted_perms := ["READ", "WRITE"] if { same_group; not admin }
granted_perms := ["READ", "WRITE", "ADMIN"] if admin

expiry_secs := 3600 if { same_group; not admin }
expiry_secs := 600 if admin
)";

Identity_t consumer(const char* group, const char* role)
{
    Identity_t identity;
    identity.spiffeId = "spiffe://test.local/group/x/role/y";
    identity.group = group;
    identity.role = role;
    return identity;
}

Decision_t decisionFor(const char* document)
{
    return readDecision(document);
}

void refuseMissingPolicy(fsdaemon::probe::Context& ctx)
{
    bool threw = false;
    try
    {
        const LocalPolicyEngine engine{"/nonexistent/policy.rego", LocalPolicyEngine::getDefaultQuery()};
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    ctx.check(threw, "a missing policy file fails at construction");
}

void checkRealRequests(fsdaemon::probe::Context& ctx)
{
    const TempFile file{PolicyRego, "daemon-policy"};
    LocalPolicyEngine engine{file.getPath(), LocalPolicyEngine::getDefaultQuery()};

    const Decision_t same = engine.decide(consumer("prod", "llm-worker"), "prod", "llm-worker");
    ctx.check(same.allow, "same-group worker is allowed");
    ctx.checkf(same.grantedPerms == 0x03U, "same-group grants READ|WRITE (0x%x)", same.grantedPerms);
    ctx.check(same.rule == "same-group", "the same-group rule is named");
    ctx.check(same.expirySecs == 3600, "same-group expiry is 3600");

    const Decision_t cross = engine.decide(consumer("dev", "llm-worker"), "prod", "llm-worker");
    ctx.check(!cross.allow, "a cross-group worker is denied");

    const Decision_t admin = engine.decide(consumer("dev", "cache-admin"), "prod", "llm-worker");
    ctx.check(admin.allow, "an admin is allowed across groups");
    ctx.checkf(admin.grantedPerms == 0x0BU, "admin grants READ|WRITE|ADMIN (0x%x)", admin.grantedPerms);
    ctx.check(admin.rule == "admin", "the admin rule is named");

    // A repeat is a cache hit and returns the same decision.
    const Decision_t again = engine.decide(consumer("prod", "llm-worker"), "prod", "llm-worker");
    ctx.check(again.allow && again.grantedPerms == same.grantedPerms, "a repeated request decides the same");
}

void checkReloadKeepsPolicy(fsdaemon::probe::Context& ctx)
{
    const TempFile file{PolicyRego, "daemon-policy"};
    LocalPolicyEngine engine{file.getPath(), LocalPolicyEngine::getDefaultQuery()};

    file.rewrite(std::string{"package "} + std::string{fsdaemon::name::FsName} + ".authz\nthis is not rego {{{\n");
    engine.reload();

    const Decision_t same = engine.decide(consumer("prod", "llm-worker"), "prod", "llm-worker");
    ctx.check(same.allow, "a bad reload keeps the policy already loaded");
}

bool passesReloadCheck(const LocalPolicyEngine& engine)
{
    try
    {
        engine.checkReload();
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

void checkReloadCheck(fsdaemon::probe::Context& ctx)
{
    const TempFile file{PolicyRego, "daemon-policy"};
    const LocalPolicyEngine engine{file.getPath(), LocalPolicyEngine::getDefaultQuery()};
    ctx.check(passesReloadCheck(engine), "a policy that compiles passes the check");

    file.rewrite(std::string{"package "} + std::string{fsdaemon::name::FsName} + ".authz\nthis is not rego {{{\n");
    ctx.check(!passesReloadCheck(engine), "a policy that will not compile fails the check");
}

void checkFailClosedReading(fsdaemon::probe::Context& ctx)
{
    ctx.check(!decisionFor("{}").allow && decisionFor("{}").reason.find("allow") != std::string::npos,
              "a result with no allow is denied");
    ctx.check(!decisionFor(R"({"allow":false})").allow, "allow:false is denied");
    ctx.check(decisionFor(R"({"allow":true,"granted_perms":"READ"})").reason.find("list") !=
                  std::string::npos,
              "a non-list granted_perms is denied for the list");
    ctx.check(decisionFor(R"({"allow":true,"granted_perms":["READ","BOGUS"]})").reason.find("unknown") !=
                  std::string::npos,
              "an unknown perm name is denied");
    const Decision_t full =
        decisionFor(R"({"allow":true,"granted_perms":["READ","WRITE"],"expiry_secs":600,"rule":"r"})");
    ctx.check(full.allow && full.grantedPerms == 0x03U && full.expirySecs == 600 && full.rule == "r",
              "a well-formed allow reads through");
}

// A document the engine would never write, and that must not read as an allow whichever way its
// two answers or its broken number happened to be taken.
void checkMalformedResult(fsdaemon::probe::Context& ctx)
{
    ctx.check(!decisionFor(R"({"allow":true,"allow":false,"granted_perms":["READ"]})").allow,
              "a key given twice is denied whichever value comes first");
    ctx.check(!decisionFor(R"({"allow":false,"allow":true,"granted_perms":["READ"]})").allow,
              "and whichever comes last");
    ctx.check(!decisionFor(R"({"allow":true,"granted_perms":["READ"],"expiry_secs":1-2})").allow,
              "a number with trailing characters is not a number");
    ctx.check(!decisionFor(R"({"allow":true,"granted_perms":["READ"],"expiry_secs":"tomorrow"})").allow,
              "an expiry that is not a number is denied rather than read as none");
    ctx.check(!decisionFor(R"({"allow":true,"granted_perms":["READ"],"expiry_secs":1e30})").allow,
              "an expiry past what an int64 holds is denied");
    ctx.check(!decisionFor(R"({"allow":true,"granted_perms":["READ"],"expiry_secs":-1})").allow,
              "a negative expiry is denied");
    ctx.check(decisionFor(R"({"allow":true,"granted_perms":["READ"],"expiry_secs":0})").allow,
              "an expiry of zero still reads through");
}

void checkUnreadableResult(fsdaemon::probe::Context& ctx)
{
    ctx.check(!decisionFor("{").allow, "an unterminated object is denied");
    ctx.check(!decisionFor("nope").allow, "a bare word is denied");
    ctx.check(!decisionFor(R"({"allow":true} junk)").allow, "trailing junk is denied");
    ctx.check(decisionFor("{").reason.find("could not read") != std::string::npos,
              "the denial says the document could not be read");
}

void checkDeepNesting(fsdaemon::probe::Context& ctx)
{
    // The reader recurses, so a deep enough document would exhaust the stack. It is refused instead.
    const std::string deep = std::string(100000, '[') + "1" + std::string(100000, ']');
    ctx.check(!decisionFor(deep.c_str()).allow, "a document built to overflow the stack is denied");
}

void checkEscapes(fsdaemon::probe::Context& ctx)
{
    const Decision_t decision = decisionFor(R"({"allow":false,"reason":"a\tb\u0041c\"d"})");
    ctx.check(decision.reason == "a\tbAc\"d", "escapes decode into the value");
}

void checkWrappedDecision(fsdaemon::probe::Context& ctx)
{
    const Decision_t wrapped =
        readDecision(R"({"result":{"allow":true,"granted_perms":["READ"]}})", "result");
    ctx.check(wrapped.allow && wrapped.grantedPerms != 0, "a wrapped decision reads through");
    ctx.check(!readDecision(R"({"other":{}})", "result").allow, "a missing wrapper member denies");
}

void checkNonAsciiQuoting(fsdaemon::probe::Context& ctx)
{
    // The same signed-char trap on the writing side: a continuation byte read as signed would be
    // escaped as a control character, splitting one character into three.
    Identity_t consumer;
    consumer.group = "\xed\x95\x9c\xea\xb8\x80";  // "한글"
    const std::string input = fsdaemon::policy::buildPolicyInput(consumer, "owner", "role");
    ctx.check(input.find("\xed\x95\x9c\xea\xb8\x80") != std::string::npos,
              "a multi-byte group goes into the input as itself");
    ctx.check(input.find("\\u00") == std::string::npos, "no byte of it is escaped as a control character");
}

void checkShippedExample(fsdaemon::probe::Context& ctx)
{
    // The example a deployment copies must answer with the object this daemon reads, under the
    // names it reads. A rename on either side shows up here rather than as an empty audit field.
    const std::ifstream source{DAEMON_EXAMPLE_POLICY};
    std::ostringstream buffer;
    buffer << source.rdbuf();
    std::string rego = buffer.str();
    for (auto position = rego.find("@FS_NAME@"); position != std::string::npos;
         position = rego.find("@FS_NAME@"))
    {
        rego.replace(position, std::string{"@FS_NAME@"}.size(), std::string{fsdaemon::name::FsName});
    }
    ctx.check(!rego.empty(), "the example policy is readable");

    const TempFile file{rego, "daemon-policy"};
    LocalPolicyEngine engine{file.getPath(), LocalPolicyEngine::getDefaultQuery()};

    const Decision_t same = engine.decide(consumer("prod", "llm-worker"), "prod", "llm-worker");
    ctx.check(same.allow, "a same-group llm-worker is allowed");
    ctx.check(same.grantedPerms != 0, "the allow carries perms");
    ctx.check(same.rule == "same-group-llm-worker", "the rule that decided is named");

    const Decision_t stranger = engine.decide(consumer("other", "stranger"), "prod", "llm-worker");
    ctx.check(!stranger.allow, "an unrelated consumer is denied");
}

void checkPackageQuery(fsdaemon::probe::Context& ctx)
{
    // What an administrator writes in config.yaml is the package, and rego reaches a rule only
    // through data. A query taken verbatim would answer nothing and refuse every request.
    const TempFile file{std::string{PolicyRego}, "daemon-policy"};
    LocalPolicyEngine engine{file.getPath(), std::string{fsdaemon::name::FsName} + ".authz"};

    const Decision_t same = engine.decide(consumer("prod", "llm-worker"), "prod", "llm-worker");
    ctx.check(same.allow, "a query naming only the package decides as the qualified one does");
    ctx.check(same.rule == "same-group", "the rule that decided is named");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            refuseMissingPolicy(ctx);
            checkRealRequests(ctx);
            checkReloadKeepsPolicy(ctx);
            checkReloadCheck(ctx);
            checkFailClosedReading(ctx);
            checkMalformedResult(ctx);
            checkUnreadableResult(ctx);
            checkDeepNesting(ctx);
            checkEscapes(ctx);
            checkWrappedDecision(ctx);
            checkNonAsciiQuoting(ctx);
            checkShippedExample(ctx);
            checkPackageQuery(ctx);
        });
}
