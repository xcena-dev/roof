// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/local_provider_test.cpp -- the local yaml identity backend, resolving this process's own
// pid against a rules file the test writes.
//
// One attest end to end, from kernel facts to an identity over a rules file, and the reload that
// keeps the last good rules when an edit will not parse.

#include <unistd.h>

#include <string>

#include "harness/creds.hpp"
#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "identity/base.hpp"
#include "identity/internal/local.hpp"
#include "model.hpp"

namespace
{

using fsdaemon::Creds_t;
using fsdaemon::Identity_t;
using fsdaemon::identity::BridgeError;
using fsdaemon::identity::LocalIdentityProvider;
using fsdaemon::probe::factsForSelf;
using fsdaemon::probe::labelsContain;
using fsdaemon::probe::TempFile;

void checkSelfResolution(fsdaemon::probe::Context& ctx)
{
    const std::string rules =
        "rules:\n"
        "  - match:\n"
        "      uid: " +
        std::to_string(::getuid()) +
        "\n"
        "    identity:\n"
        "      group: prod\n"
        "      role: llm-worker\n";
    const TempFile file{rules, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    const Creds_t facts = factsForSelf();
    const Identity_t identity = provider.attest(::getpid(), facts);
    ctx.check(identity.group == "prod", "the matching rule sets the group");
    ctx.check(identity.role == "llm-worker", "the matching rule sets the role");
    ctx.check(labelsContain(identity.selectors, "unix:uid:" + std::to_string(::getuid())),
              "the matched selector is recorded");
}

void checkUnnamedKindRuleDrop(fsdaemon::probe::Context& ctx)
{
    // The default set is the uid alone, so the gid rule above is a real kind this deployment did
    // not subscribe to. It is dropped and the uid rule below it still resolves.
    const std::string rules =
        "rules:\n"
        "  - match:\n"
        "      gid: " +
        std::to_string(::getgid()) +
        "\n"
        "    identity:\n"
        "      group: by-gid\n"
        "  - match:\n"
        "      uid: " +
        std::to_string(::getuid()) +
        "\n"
        "    identity:\n"
        "      group: by-uid\n";
    const TempFile file{rules, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "by-uid", "a rule naming a kind outside the configured set is dropped");
    ctx.check(provider.getKnownIdentities().size() == 1, "only the rule the set supports is loaded");
}

void checkHashingSetNeedsWorker(fsdaemon::probe::Context& ctx)
{
    const LocalIdentityProvider plain{"", {}};
    ctx.check(!plain.needsWorker(), "the default set reads only what the kernel sent, so the loop answers");

    const LocalIdentityProvider hashing{"", {"uid", "sha256"}};
    ctx.check(hashing.needsWorker(), "a set carrying sha256 reads the executable, so a worker answers");
}

void refuseNoMatchingRule(fsdaemon::probe::Context& ctx)
{
    const std::string rules =
        "rules:\n"
        "  - match:\n"
        "      uid: 999999\n"
        "    identity:\n"
        "      group: nobody\n";
    const TempFile file{rules, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    bool threw = false;
    try
    {
        static_cast<void>(provider.attest(::getpid(), factsForSelf()));
    }
    catch (const BridgeError&)
    {
        threw = true;
    }
    ctx.check(threw, "a pid no rule matches is refused");
}

void checkSpiffeGroupRole(fsdaemon::probe::Context& ctx)
{
    const std::string rules =
        "rules:\n"
        "  - match:\n"
        "      uid: " +
        std::to_string(::getuid()) +
        "\n"
        "    identity:\n"
        "      spiffe_id: spiffe://test.local/group/g9/role/reader\n";
    const TempFile file{rules, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "g9" && identity.role == "reader",
              "group and role fill in from the spiffe id");
}

void checkReloadKeepsRules(fsdaemon::probe::Context& ctx)
{
    const std::string good =
        "rules:\n"
        "  - match:\n"
        "      uid: " +
        std::to_string(::getuid()) +
        "\n"
        "    identity:\n"
        "      group: prod\n";
    const TempFile file{good, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    // Overwrite with a rule whose uid will not parse, so the reload throws inside and keeps the old.
    const std::string bad =
        "rules:\n"
        "  - match:\n"
        "      uid: not-a-number\n"
        "    identity:\n"
        "      group: broken\n";
    file.rewrite(bad);
    provider.reload();

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "prod", "a bad reload keeps the rules already loaded");
}

// Writes @edit over the rules @file already loaded into @provider and reloads, so a case asserts
// what a bad edit costs. The good rules name this task, so a retained load still resolves.
void overwriteAndReload(LocalIdentityProvider& provider, const TempFile& file, const std::string& edit)
{
    file.rewrite(edit);
    provider.reload();
}

std::string goodRules()
{
    return "rules:\n"
           "  - match:\n"
           "      uid: " +
           std::to_string(::getuid()) +
           "\n"
           "    identity:\n"
           "      group: prod\n";
}

void checkTopLevelKeysExcluded(fsdaemon::probe::Context& ctx)
{
    // A stray key sits before the block and default sits after it, both at the block's own column.
    // A scan that read indentation only for the dash would fold default into the last identity.
    const std::string rules = "version: 1\n" + goodRules() + "default: deny\n";
    const TempFile file{rules, "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "prod", "a key at the block's column ends the block");
}

void refuseMisspelledSection(fsdaemon::probe::Context& ctx)
{
    const TempFile file{goodRules(), "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    const std::string typo =
        "rules:\n"
        "  - match:\n"
        "      uid: " +
        std::to_string(::getuid()) +
        "\n"
        "    identiy:\n"
        "      group: silent\n";
    overwriteAndReload(provider, file, typo);

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "prod", "a misspelled section refuses the file rather than losing it");
}

void refuseUnspelledKind(fsdaemon::probe::Context& ctx)
{
    const TempFile file{goodRules(), "daemon-rules"};
    LocalIdentityProvider provider{file.getPath(), {}};

    // uidd spells no kind at all, which is a typo. A kind the set does not carry is the separate
    // case checkUnnamedKindRuleDrop covers, and that one drops only the rule.
    const std::string typo =
        "rules:\n"
        "  - match:\n"
        "      uidd: 1000\n"
        "    identity:\n"
        "      group: silent\n";
    overwriteAndReload(provider, file, typo);

    const Identity_t identity = provider.attest(::getpid(), factsForSelf());
    ctx.check(identity.group == "prod", "a selector name no kind spells refuses the file");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkSelfResolution(ctx);
            checkHashingSetNeedsWorker(ctx);
            refuseNoMatchingRule(ctx);
            checkUnnamedKindRuleDrop(ctx);
            checkSpiffeGroupRole(ctx);
            checkReloadKeepsRules(ctx);
            checkTopLevelKeysExcluded(ctx);
            refuseMisspelledSection(ctx);
            refuseUnspelledKind(ctx);
        });
}
