// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/handler.cpp -- see handler.hpp.

#include "serve/handler.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "audit/audit.hpp"
#include "identity/base.hpp"
#include "model.hpp"
#include "observe/stat.hpp"
#include "policy/base.hpp"
#include "serve/channel.hpp"
#include "serve/worker_pool.hpp"
#include "turn/service.hpp"
#include "wire/codec.hpp"
#include "wire/protocol.hpp"

namespace
{

// The credentials an attest request carries. The kernel holds the caller for the whole upcall, so
// these are its word and nothing re-reads them.
fsdaemon::Creds_t readCreds(const fsdaemon::FsDaemonAttestRequest& request)
{
    fsdaemon::Creds_t facts{};
    facts.uid = request.owner_uid;
    facts.gid = request.owner_gid;
    facts.startBoottimeNs = request.owner_start_boottime_ns;
    facts.exePath = fsdaemon::wire::readName(request.owner_exe_path);
    facts.exeInodeIno = request.owner_exe_inode_ino;
    facts.exeInodeDev = request.owner_exe_inode_dev;
    return facts;
}

// The same for an access request, which describes the consumer rather than the owner.
fsdaemon::Creds_t readCreds(const fsdaemon::FsDaemonAccessRequest& request)
{
    fsdaemon::Creds_t facts{};
    facts.uid = request.consumer_uid;
    facts.gid = request.consumer_gid;
    facts.startBoottimeNs = request.consumer_start_boottime_ns;
    facts.exePath = fsdaemon::wire::readName(request.consumer_exe_path);
    facts.exeInodeIno = request.consumer_exe_inode_ino;
    facts.exeInodeDev = request.consumer_exe_inode_dev;
    return facts;
}

// The fields every audit line carries, whatever else the caller goes on to fill in.
fsdaemon::audit::Record_t startRecord(fsdaemon::audit::Record_t::Kind kind, std::uint64_t seq, std::uint32_t pid,
                                      std::chrono::steady_clock::time_point began)
{
    fsdaemon::audit::Record_t entry{};
    entry.kind = kind;
    entry.seq = seq;
    entry.pid = pid;
    entry.latency = std::chrono::steady_clock::now() - began;
    return entry;
}

}  // namespace

namespace fsdaemon::serve
{

std::uint64_t RequestHandler::markStage() const noexcept
{
    return observe::markStart(stat_);
}

std::uint64_t RequestHandler::foldStage(std::string_view stage, std::uint64_t beganNs) noexcept
{
    return observe::addStage(stat_, stage, beganNs);
}

void RequestHandler::handleAttest(FsChannel& channel, const Frame_t& frame) noexcept
{
    const auto request = wire::decode<FsDaemonAttestRequest>(frame.payload);
    if (!request)
    {
        channel.sendAttestResponse(frame.header.seq, -EINVAL, "", "");
        return;
    }

    const auto facts = readCreds(*request);
    const auto began = std::chrono::steady_clock::now();
    try
    {
        auto stamp = markStage();
        const auto identity = identity_.attest(static_cast<pid_t>(request->owner_pid), facts);  // NOLINT(misc-include-cleaner) pid_t arrives with identity::IdentityProvider::attest()'s declaration in identity/base.hpp
        // The wire keeps FS_DAEMON_IDENT_LEN - 1 bytes of a label. One that would be cut would
        // reach the kernel as another tenant's prefix, and an empty one names nobody, so both
        // are denials here rather than truncations on the way out.
        constexpr std::uint64_t LabelMax = FS_DAEMON_IDENT_LEN - 1;
        const auto isSendable = [](const std::string& label)
        {
            return !label.empty() && label.size() <= LabelMax;
        };
        if (!isSendable(identity.group) || !isSendable(identity.role))
        {
            throw identity::BridgeError{std::string{"identity label empty or longer than "}
                                            .append(std::to_string(LabelMax))
                                            .append(" bytes: group='")
                                            .append(identity.group)
                                            .append("' role='")
                                            .append(identity.role)
                                            .append("'")};
        }
        stamp = foldStage("attest", stamp);
        channel.sendAttestResponse(frame.header.seq, 0, identity.group, identity.role);
        stamp = foldStage("reply", stamp);
        if (audit_ != nullptr)
        {
            auto entry = startRecord(audit::Record_t::Kind::Attest, frame.header.seq, request->owner_pid, began);
            entry.hasIdentity = true;
            entry.group = identity.group;
            entry.role = identity.role;
            entry.spiffeId = identity.spiffeId;
            audit_->record(entry);
        }
        foldStage("audit", stamp);
    }
    catch (const identity::BridgeError& denied)
    {
        channel.sendAttestResponse(frame.header.seq, -EACCES, "", "");
        if (audit_ != nullptr)
        {
            auto entry = startRecord(audit::Record_t::Kind::Attest, frame.header.seq, request->owner_pid, began);
            entry.extra = denied.what();
            audit_->record(entry);
        }
    }
}

void RequestHandler::handleAccess(FsChannel& channel, const Frame_t& frame) noexcept
{
    if (policy_ == nullptr)
    {
        // No policy engine, so an access is denied rather than answered wrong.
        channel.sendAccessResponse(frame.header.seq, -EACCES, 0, 0);
        return;
    }

    const auto request = wire::decode<FsDaemonAccessRequest>(frame.payload);
    if (!request)
    {
        channel.sendAccessResponse(frame.header.seq, -EINVAL, 0, 0);
        return;
    }

    const auto facts = readCreds(*request);
    const auto ownerGroup = wire::readName(request->owner_group);
    const auto ownerRole = wire::readName(request->owner_role);

    const auto began = std::chrono::steady_clock::now();
    try
    {
        auto stamp = markStage();
        const auto consumer = identity_.attest(static_cast<pid_t>(request->consumer_pid), facts);
        stamp = foldStage("attest", stamp);
        const auto decision = policy_->decide(consumer, ownerGroup, ownerRole);
        stamp = foldStage("decide", stamp);
        if (decision.allow)
        {
            channel.sendAccessResponse(frame.header.seq, 0, decision.grantedPerms,
                                       decision.expirySecs);
        }
        else
        {
            channel.sendAccessResponse(frame.header.seq, -EACCES, 0, 0);
        }
        stamp = foldStage("reply", stamp);
        if (audit_ != nullptr)
        {
            auto entry = startRecord(audit::Record_t::Kind::Access, frame.header.seq, request->consumer_pid, began);
            entry.hasIdentity = true;
            entry.group = consumer.group;
            entry.role = consumer.role;
            entry.spiffeId = consumer.spiffeId;
            entry.hasDecision = true;
            entry.allow = decision.allow;
            entry.grantedPerms = decision.grantedPerms;
            entry.expiry = std::chrono::seconds{decision.expirySecs};
            entry.rule = decision.rule;
            entry.reason = decision.reason;
            audit_->record(entry);
        }
        foldStage("audit", stamp);
    }
    catch (const identity::BridgeError& denied)
    {
        channel.sendAccessResponse(frame.header.seq, -EACCES, 0, 0);
        if (audit_ != nullptr)
        {
            auto entry = startRecord(audit::Record_t::Kind::Access, frame.header.seq, request->consumer_pid, began);
            entry.extra = denied.what();
            audit_->record(entry);
        }
    }
}

RequestHandler::RequestHandler(const Services_t& services) noexcept
    : identity_{services.identity},
      policy_{services.policy},
      turn_{services.turn},
      pool_{services.pool},
      audit_{services.audit},
      stat_{services.stat},
      checkSettings_{services.checkSettings},
      usesWorker_{identity_.needsWorker() || (policy_ != nullptr && policy_->needsWorker())}
{
}

void RequestHandler::handle(FsChannel& channel, const Frame_t& frame) noexcept
{
    if (pool_ == nullptr || !usesWorker_)
    {
        // Nothing on this path can park, so the caller's thread answers and pays no wake.
        answer(channel, frame);
        return;
    }

    // A turn is the loop's own: only the thread that took one may give it back, and nothing on this
    // path waits, so handing it to a worker would buy nothing and cost two wakes.
    if (frame.header.type == FS_DAEMON_MSG_LOCK_REQUEST ||
        frame.header.type == FS_DAEMON_MSG_UNLOCK_REQUEST)
    {
        answer(channel, frame);
        return;
    }

    // A remote identity or policy call parks for seconds, and the kernel times each upcall from
    // when it queued it, so the loop hands the whole answer over and reads the next frame. The
    // frame is copied because the loop's buffer is gone by then.
    pool_->submit(
        [this, target = &channel, carried = frame]
        {
            answer(*target, carried);
        });
}

void RequestHandler::answer(FsChannel& channel, const Frame_t& frame) noexcept
{
    switch (frame.header.type)
    {
        case FS_DAEMON_MSG_ATTEST_REQUEST:
            handleAttest(channel, frame);
            break;
        case FS_DAEMON_MSG_ACCESS_REQUEST:
            handleAccess(channel, frame);
            break;
        case FS_DAEMON_MSG_LOCK_REQUEST:
            if (!handleLock(channel, frame))
            {
                return;
            }
            break;
        case FS_DAEMON_MSG_UNLOCK_REQUEST:
            // Served but not answered, so it is not one of the answers a report counts.
            handleUnlock(frame);
            return;
        default:
            // An unknown or helper-bound type is dropped: there is nothing to answer.
            return;
    }
    observe::recordAnswer(stat_);
}

bool RequestHandler::handleLock(FsChannel& channel, const Frame_t& frame) noexcept
{
    if (turn_ == nullptr)
    {
        channel.sendLockResponse(frame.header.seq, FS_DAEMON_MSG_LOCK_RESPONSE, -EOPNOTSUPP);
        return true;
    }
    const auto request = wire::decode<FsDaemonLockRequest>(frame.payload);
    if (!request)
    {
        channel.sendLockResponse(frame.header.seq, FS_DAEMON_MSG_LOCK_RESPONSE, -EINVAL);
        return true;
    }

    const auto domain = wire::readName(request->domain);
    const std::chrono::milliseconds maxWait{FS_DAEMON_LOCK_MAX_WAIT_MS};
    const std::chrono::milliseconds asked{static_cast<std::uint32_t>(request->timeout_ms)};
    const auto isCapped = asked == std::chrono::milliseconds::zero() || asked > maxWait;
    const auto timeout = isCapped ? maxWait : asked;

    const auto began = markStage();
    try
    {
        if (turn_->tryAcquireHere(domain))
        {
            foldStage("lock_acquire", began);
            channel.sendLockResponse(frame.header.seq, FS_DAEMON_MSG_LOCK_RESPONSE, 0);
            return true;
        }
    }
    catch (const turn::TurnUnavailable&)
    {
        foldStage("lock_acquire", began);
        channel.sendLockResponse(frame.header.seq, FS_DAEMON_MSG_LOCK_RESPONSE, -ENOENT);
        return true;
    }

    // Busy. The wait goes inside the region's own queue rather than back out to this loop, so the
    // node that asked first keeps its place, and the loop answers everyone else meanwhile.
    turn_->awaitDomain(domain, timeout,
                       [target = &channel, seq = frame.header.seq, stat = stat_, began](std::int32_t status)
                       {
                           // Counted before the reply goes out, so whoever saw the reply sees the
                           // count. The recorder alone is captured: the turn outlives this handler.
                           observe::addStage(stat, "lock_acquire", began);
                           observe::recordAnswer(stat);
                           target->sendLockResponse(seq, FS_DAEMON_MSG_LOCK_RESPONSE, status);
                       });
    return false;
}

// Nothing is sent back: the kernel does not wait for a release, and a LOCK it queues afterwards
// reaches this loop after the release has run. A domain this node does not hold is a no-op.
void RequestHandler::handleUnlock(const Frame_t& frame) noexcept
{
    if (turn_ == nullptr)
    {
        return;
    }
    const auto request = wire::decode<FsDaemonUnlockRequest>(frame.payload);
    if (!request)
    {
        return;
    }

    try
    {
        static_cast<void>(turn_->release(wire::readName(request->domain)));
    }
    catch (const turn::TurnUnavailable&)
    {
        // @expected: an UNLOCK has no answer to carry a refusal, and a turn already gone is released.
    }
}

std::uint64_t RequestHandler::getCapabilities() const noexcept
{
    std::uint64_t caps = FS_DAEMON_CAP_ATTEST_OWNER | FS_DAEMON_CAP_ACCESS_REQUEST;
    if (turn_ != nullptr && pool_ != nullptr)
    {
        caps |= FS_DAEMON_CAP_LOCK;
    }
    return caps;
}

void RequestHandler::reloadBackends() noexcept
{
    try
    {
        if (checkSettings_)
        {
            checkSettings_();
        }
        identity_.checkReload();
        if (policy_ != nullptr)
        {
            policy_->checkReload();
        }
        identity_.reload();
        if (policy_ != nullptr)
        {
            policy_->reload();
        }
    }
    catch (const std::exception& refused)
    {
        std::fprintf(stderr, "[config] reload refused (%s); everything loaded is kept\n", refused.what());
    }
    catch (...)  // NOLINT(bugprone-empty-catch) both reloads keep what was already loaded on failure
    {
    }
}

}  // namespace fsdaemon::serve
