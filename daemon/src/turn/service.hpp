// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// turn/service.hpp -- the cross-node lock the kernel cannot take itself.
//
// A region's metadata turn is served by a peer, and a kthread cannot be one, so the kernel asks this
// daemon. The interface is here, in the core, so the serving code names it without linking cme; the
// cme-backed implementation lives behind DAEMON_ENABLE_TURN.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace fsdaemon::turn
{

// The lock service is absent, or the region refused the session, or a domain name is not one this
// node serves. The lock handler turns this into -ENOENT rather than letting it escape.
class TurnUnavailable : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class TurnService
{
public:
    TurnService() = default;
    TurnService(const TurnService&) = delete;
    TurnService(TurnService&&) = delete;
    TurnService& operator=(const TurnService&) = delete;
    TurnService& operator=(TurnService&&) = delete;
    virtual ~TurnService() = default;

    // What a caller gets told once a domain it waited for is settled: 0 when it has it, -EBUSY when
    // the deadline passed first, -ENOENT when the region cannot serve that name.
    using Answer = std::function<void(std::int32_t status)>;

    // Takes @domain if it can do so within a slice short enough for the serve loop to spend. False
    // when it could not, and the caller then has awaitDomain take over.
    //
    // PRECONDITION: the serve loop is the only thread that calls this and the only one that calls
    // release for what it took. A take holds a lock that only its own thread may give back.
    [[nodiscard]] virtual bool tryAcquireHere(const std::string& domain) = 0;

    // Waits for @domain up to @timeout on a thread of this service's own, then calls @answer with
    // the outcome. The wait happens inside the region's own queue rather than as a retry from
    // outside it, so a node that asked first is not overtaken by one that asked later.
    virtual void awaitDomain(const std::string& domain, std::chrono::nanoseconds timeout, Answer answer) = 0;

    // Gives @domain back, on whichever thread took it. False when this node was not holding it,
    // which a repeated unlock reads as.
    [[nodiscard]] virtual bool release(const std::string& domain) = 0;
};

}  // namespace fsdaemon::turn
