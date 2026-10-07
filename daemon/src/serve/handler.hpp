// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/handler.hpp -- what one request off the wire turns into.
//
// The kernel sends four requests: attest a pid, decide an access, take and give back a lock. This
// answers each and writes the response on the channel. Attest and access are resolved when a policy
// engine is present; without one an access denies fail-closed. A lock is served when a turn service
// and a worker pool are present, so the wait for a peer on another node runs off the loop.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace fsdaemon::identity
{
class IdentityProvider;
}

namespace fsdaemon::policy
{
class PolicyEngine;
}

namespace fsdaemon::turn
{
class TurnService;
}

namespace fsdaemon::audit
{
class AuditLogger;
}

namespace fsdaemon::observe
{
class Stat;
}

namespace fsdaemon::serve
{

class FsChannel;
class WorkerPool;
struct Frame_t;

// What a handler answers with. Only the identity backend is required.
struct Services_t
{
    identity::IdentityProvider& identity;
    // Without one, an access denies fail-closed.
    policy::PolicyEngine* policy{nullptr};
    // A lock needs both, because its wait runs on the pool so the loop keeps answering.
    turn::TurnService* turn{nullptr};
    WorkerPool* pool{nullptr};
    // Without one, nothing is logged.
    audit::AuditLogger* audit{nullptr};
    // Without one, no clock is read.
    observe::Stat* stat{nullptr};
    // Throws when the settings file holds an edit a reload cannot apply. Without one, a reload
    // checks only the backends.
    std::function<void()> checkSettings{};
};

class RequestHandler
{
public:
    explicit RequestHandler(const Services_t& services) noexcept;

    // Takes @frame off the loop. With a pool the answer runs on a worker, so a remote identity or
    // policy call cannot stall the next request. Never throws: a thrown exception on the serving
    // path would take the loop down.
    void handle(FsChannel& channel, const Frame_t& frame) noexcept;

    // The capability bits the HELLO advertises. Attest and access are always answered; lock is added
    // only once a turn service backs it, so the kernel does not queue a lock nobody will take.
    [[nodiscard]] std::uint64_t getCapabilities() const noexcept;

    // Re-reads the identity backend and the policy, on a SIGHUP. Every check runs before anything is
    // swapped, so a refused edit leaves all of what was loaded in place.
    void reloadBackends() noexcept;

private:
    // Answers one frame, on whichever thread got here.
    void answer(FsChannel& channel, const Frame_t& frame) noexcept;

    // A stamp, and the fold that turns one into the next stage's. Both do nothing and read no clock
    // when no recorder is attached, so an uninstrumented run pays one null check.
    [[nodiscard]] std::uint64_t markStage() const noexcept;
    std::uint64_t foldStage(std::string_view stage, std::uint64_t beganNs) noexcept;

    void handleAttest(FsChannel& channel, const Frame_t& frame) noexcept;
    void handleAccess(FsChannel& channel, const Frame_t& frame) noexcept;
    // True when the reply went out here. False when the wait went to the turn service, whose
    // callback then sends the reply and finishes the count, so the loop must not count it now.
    [[nodiscard]] bool handleLock(FsChannel& channel, const Frame_t& frame) noexcept;
    void handleUnlock(const Frame_t& frame) noexcept;

    identity::IdentityProvider& identity_;
    policy::PolicyEngine* policy_;
    turn::TurnService* turn_;
    WorkerPool* pool_;
    audit::AuditLogger* audit_;
    observe::Stat* stat_;
    std::function<void()> checkSettings_;
    bool usesWorker_{false};
};

}  // namespace fsdaemon::serve
