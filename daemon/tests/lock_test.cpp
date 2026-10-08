// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tests/lock_test.cpp -- the cross-node turn, over a file-backed cme region.
//
// Two CmeTurn sessions on one region stand in for two nodes: cme's ownership is per peer, so they
// exclude each other. The test never touches the kernel or the char device; it drives the turn
// service directly and checks acquire, release, contention and the same-node double-lock refusal.

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "cme/shared.hpp"
#include "harness/probe.hpp"
#include "harness/temp_file.hpp"
#include "turn/internal/cme_turn.hpp"

namespace
{

using fsdaemon::probe::TempFile;
using fsdaemon::turn::CmeTurn;

std::string formatRegion(const std::string& path)
{
    const std::string uri = std::string{"file:"} + path;
    cme::Session::FormatOpts_t opts;
    opts.maxPeers = 4;
    opts.maxDomains = 4;
    cme::Session::format(uri, opts);
    return uri;
}

// What the serve loop does with a busy domain: ask again until it is free or the deadline passes.
// One try can also miss on a loaded machine, since it spends a slice rather than waiting, so a case
// that wants the domain asks the way the loop does.
bool takeWithin(CmeTurn& node, const std::string& domain, std::chrono::milliseconds within)
{
    const auto deadline = std::chrono::steady_clock::now() + within;
    do
    {
        if (node.tryAcquireHere(domain))
        {
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// Takes @domain on the waiter thread, the way a domain the loop found busy is taken, and returns the
// status the waiter answered with.
std::int32_t takeOnWaiter(CmeTurn& node, const std::string& domain)
{
    std::promise<std::int32_t> answered;
    node.awaitDomain(domain, std::chrono::seconds{2},
                     [&answered](std::int32_t status)
                     {
                         answered.set_value(status);
                     });
    return answered.get_future().get();
}

void checkTurnExclusion(fsdaemon::probe::Context& ctx)
{
    const TempFile region{"", "daemon-turn"};
    const std::string uri = formatRegion(region.getPath());
    const std::vector<std::string> domains{"turn.d"};

    CmeTurn nodeA{uri, domains};
    CmeTurn nodeB{uri, domains};

    ctx.check(takeWithin(nodeA, "turn.d", std::chrono::seconds{2}), "node A takes the domain");
    ctx.check(nodeA.holds("turn.d"), "node A holds it");
    ctx.check(!nodeB.tryAcquireHere("turn.d"), "node B cannot take it while A holds it");

    ctx.check(nodeA.release("turn.d"), "node A gives it back");
    ctx.check(!nodeA.holds("turn.d"), "node A no longer holds it");
    ctx.check(takeWithin(nodeB, "turn.d", std::chrono::seconds{2}), "node B takes it once free");
    ctx.check(nodeB.release("turn.d"), "node B gives it back");
}

void refuseDoubleLock(fsdaemon::probe::Context& ctx)
{
    const TempFile region{"", "daemon-turn"};
    const std::string uri = formatRegion(region.getPath());
    CmeTurn node{uri, std::vector<std::string>{"turn.d"}};

    ctx.check(takeWithin(node, "turn.d", std::chrono::seconds{2}), "the domain is taken");
    ctx.check(!node.tryAcquireHere("turn.d"),
              "a second lock for a domain already held here is refused");
    ctx.check(node.release("turn.d"), "the domain is given back");
    ctx.check(!node.release("turn.d"), "a second unlock reports it was not held");
}

// A waiter-held domain is dropped on the waiter, so an unlock returns before the drop runs. A second
// unlock in that window must read as a repeat: a second queued drop would land on whatever lock the
// loop takes once the first drop frees the domain.
void checkRepeatedUnlock(fsdaemon::probe::Context& ctx)
{
    const TempFile region{"", "daemon-turn"};
    const std::string uri = formatRegion(region.getPath());
    CmeTurn node{uri, std::vector<std::string>{"turn.d", "turn.e"}};

    ctx.check(takeOnWaiter(node, "turn.d") == 0, "the waiter takes the domain");
    ctx.check(node.release("turn.d"), "the first unlock hands the drop to the waiter");
    ctx.check(!node.release("turn.d"), "a second unlock before the drop ran reads as a repeat");

    ctx.check(takeWithin(node, "turn.d", std::chrono::seconds{2}), "the loop takes the domain once dropped");
    // The waiter runs its queue in order, so an answer for another domain means every drop queued
    // before it has run.
    ctx.check(takeOnWaiter(node, "turn.e") == 0, "the waiter drains what it had queued");
    ctx.check(node.holds("turn.d"), "the loop's lock survives the waiter draining its queue");

    ctx.check(node.release("turn.e"), "the second domain is given back");
    ctx.check(node.release("turn.d"), "the loop's lock is given back");
}

}  // namespace

int main()
{
    return fsdaemon::probe::runProbe(
        [](fsdaemon::probe::Context& ctx)
        {
            checkTurnExclusion(ctx);
            refuseDoubleLock(ctx);
            checkRepeatedUnlock(ctx);
        });
}
