// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/spire_provider_test.cpp -- the SPIRE provider over a canned fetcher, the network-free part of
// test_spire_bridge.py.
//
// The SVID fetch is a seam, so this drives attest without a SPIRE agent. It checks that uid derives
// the label the agent is asked for, that the returned id parses into group and role, and that a
// fetch error is a denial. The gRPC fetcher itself is exercised only by building it under the flag.

#include <cstdint>
#include <string>
#include <vector>

#include "harness/probe.hpp"
#include "identity/base.hpp"
#include "identity/internal/spire.hpp"
#include "model.hpp"

namespace
{

using fsdaemon::Creds_t;
using fsdaemon::Identity_t;
using fsdaemon::identity::BridgeError;
using fsdaemon::identity::SpireIdentityProvider;
using fsdaemon::identity::SvidFetcher;

// Kernel facts for a caller, so the provider builds its creds from these and opens no /proc.
Creds_t factsFor(std::uint32_t uid)
{
    Creds_t facts;
    facts.uid = uid;
    facts.gid = 100;
    facts.exePath = "/usr/bin/consumer";
    return facts;
}

void checkUidLabelAndIdParsing(fsdaemon::probe::Context& ctx)
{
    std::vector<std::string> seen;
    const SvidFetcher fetcher = [&seen](const std::vector<std::string>& labels)
    {
        seen = labels;
        return std::string{"spiffe://test.local/group/eng/role/reader"};
    };

    SpireIdentityProvider provider{{}, fetcher};
    const Identity_t who = provider.attest(4321, factsFor(1000));

    ctx.check(seen.size() == 1 && seen.front() == "unix:uid:1000",
              "the uid selector label is what the agent is asked for");
    ctx.check(who.spiffeId == "spiffe://test.local/group/eng/role/reader", "the SVID id is carried");
    ctx.check(who.group == "eng" && who.role == "reader", "the id parses into group and role");
}

void checkFetchErrorDenial(fsdaemon::probe::Context& ctx)
{
    const SvidFetcher down = [](const std::vector<std::string>&) -> std::string
    {
        throw BridgeError{"agent returned 0 SVIDs"};
    };

    SpireIdentityProvider provider{{}, down};
    bool threw = false;
    try
    {
        static_cast<void>(provider.attest(4321, factsFor(1000)));
    }
    catch (const BridgeError&)
    {
        threw = true;
    }
    ctx.check(threw, "an agent error is a BridgeError the attest path denies on");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkUidLabelAndIdParsing(ctx);
            checkFetchErrorDenial(ctx);
        });
}
