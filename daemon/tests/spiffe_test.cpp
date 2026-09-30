// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/spiffe_test.cpp -- the SPIFFE id parse table.

#include "identity/internal/spiffe.hpp"

#include <string>
#include <utility>

#include "harness/probe.hpp"
#include "identity/base.hpp"
#include "model.hpp"

namespace
{

using fsdaemon::Creds_t;
using fsdaemon::Identity_t;
using fsdaemon::identity::assembleSpiffeId;
using fsdaemon::identity::BridgeError;
using fsdaemon::identity::parseSpiffePath;

void checkSpiffeCase(fsdaemon::probe::Context& ctx, const char* input, const char* group, const char* role)
{
    const auto parsed = parseSpiffePath(input);
    ctx.checkf(parsed.first == group && parsed.second == role, "%s -> (%s, %s)", input,
               parsed.first.c_str(), parsed.second.c_str());
}

void parseTable(fsdaemon::probe::Context& ctx)
{
    checkSpiffeCase(ctx, "spiffe://test.local/group/g1/role/reader", "g1", "reader");
    checkSpiffeCase(ctx, "spiffe://test.local/group/g1", "g1", "");
    checkSpiffeCase(ctx, "spiffe://test.local/role/writer", "", "writer");
    checkSpiffeCase(ctx, "spiffe://test.local/node/n1", "", "");
    checkSpiffeCase(ctx, "spiffe://test.local/group", "", "");
    checkSpiffeCase(ctx, "spiffe://test.local/node/n1/role/writer", "", "writer");
    checkSpiffeCase(ctx, "not-a-uri", "", "");
    checkSpiffeCase(ctx, "spiffe://test.local", "", "");
}

void checkAssembleAndReject(fsdaemon::probe::Context& ctx)
{
    ctx.check(assembleSpiffeId("test.local", "/group/g1/role/reader") ==
                  "spiffe://test.local/group/g1/role/reader",
              "an id assembles from a trust domain and a path");
    ctx.check(assembleSpiffeId("test.local", "group/g1") == "spiffe://test.local/group/g1",
              "a path missing its leading slash gets one");

    bool threw = false;
    try
    {
        static_cast<void>(assembleSpiffeId("", "/group/g1"));
    }
    catch (const BridgeError&)
    {
        threw = true;
    }
    ctx.check(threw, "an empty trust domain is refused");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            parseTable(ctx);
            checkAssembleAndReject(ctx);
        });
}
