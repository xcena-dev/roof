// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// turn/internal/cme_turn.hpp -- the cme-backed turn service: one peer slot per region, and the locks through it.
//
// cme's ownership token is per peer, so this session excludes other nodes and not other askers behind
// it. A held lock spans two requests, so its Guard is kept in a map until unlock, and a Guard is
// released by the thread that took it: cme's Guard holds a unique_lock, which only its locker may
// unlock. Two threads take them here. The serve loop takes a domain that is free, which costs it a
// slice and no handoff. A domain that is busy goes to the waiter thread, which parks inside the
// region's own queue and answers from there, so waiting costs one handoff and not the loop's whole
// pass.

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared_session.hpp"
#include "turn/service.hpp"

namespace fsdaemon::turn
{

class CmeTurn : public TurnService
{
public:
    // Opens the cme region @uri under the mode its mapping calls for, takes this node's peer slot, and
    // creates or joins each of @domains. Throws TurnUnavailable when the region cannot be opened.
    CmeTurn(std::string uri, const std::vector<std::string>& domains);
    ~CmeTurn() override;

    [[nodiscard]] bool tryAcquireHere(const std::string& domain) override;
    void awaitDomain(const std::string& domain, std::chrono::nanoseconds timeout, Answer answer) override;
    [[nodiscard]] bool release(const std::string& domain) override;

    // Whether this node holds @domain right now.
    [[nodiscard]] bool holds(const std::string& domain) const;

private:
    // Which of the two threads took a domain, and so which one may give it back. cme's Guard holds a
    // unique_lock, which only its locker may unlock, so this decides where a release runs.
    enum class Taker
    {
        Loop,
        Waiter,
    };

    // Takes @domain within @timeout and records which thread may give it back. Both takes are this
    // one call under a different thread and a different deadline.
    [[nodiscard]] bool takeDomain(const std::string& domain, std::chrono::nanoseconds timeout, Taker taker);

    void runWaiter();
    void stopWaiter() noexcept;
    void post(std::function<void()> work);
    void openSession(const std::vector<std::string>& domains);
    void ensureDomain(const std::string& domain);

    std::string uri_;
    std::optional<cme::SharedSession> session_;

    // The Guard and the thread that may drop it. Covered by heldGuard_, because the loop and the
    // waiter both reach it.
    struct Held_t
    {
        cme::SharedSession::Guard guard;
        Taker taker{Taker::Loop};
        // An unlock has queued this guard's drop on the waiter. A second unlock that arrives before
        // the drop runs is a repeat, and must not queue a drop that would take the guard after this one.
        bool releasing{false};
    };
    mutable std::mutex heldGuard_;
    std::map<std::string, Held_t> held_;

    // The one thread that parks for a busy domain, and the queue that reaches it.
    std::mutex queueGuard_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> work_;
    bool stopping_{false};
    std::thread waiter_;
};

}  // namespace fsdaemon::turn
