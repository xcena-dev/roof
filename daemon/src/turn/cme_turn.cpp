// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// turn/cme_turn.cpp -- see internal/cme_turn.hpp.

#include "turn/internal/cme_turn.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "cme/shared_session.hpp"

namespace fsdaemon::turn
{

namespace
{

// What the serve loop spends before handing a busy domain to the waiter. Zero cannot be it: that
// leaves nothing for the inter-node half, which takes single-digit microseconds even when free.
constexpr auto TryBudget = std::chrono::microseconds{50};

}  // namespace

CmeTurn::CmeTurn(std::string uri, const std::vector<std::string>& domains)
    : uri_{std::move(uri)}
{
    waiter_ = std::thread{[this]
                          {
                              runWaiter();
                          }};
    try
    {
        openSession(domains);
    }
    catch (...)
    {
        stopWaiter();
        throw;
    }
}

CmeTurn::~CmeTurn()
{
    // This runs on the thread that built the service, which is the serve loop, so what the loop took
    // goes here and what the waiter took goes to the waiter.
    {
        const std::lock_guard guard{heldGuard_};
        for (auto entry = held_.begin(); entry != held_.end();)
        {
            entry = entry->second.taker == Taker::Waiter ? std::next(entry) : held_.erase(entry);
        }
    }
    post(
        [this]
        {
            const std::lock_guard guard{heldGuard_};
            held_.clear();
            session_.reset();
        });
    stopWaiter();
}

bool CmeTurn::takeDomain(const std::string& domain, std::chrono::nanoseconds timeout, Taker taker)
{
    {
        const std::lock_guard guard{heldGuard_};
        if (held_.find(domain) != held_.end())
        {
            // Already held here: the asker did not serialise its own node. Refused, not queued.
            return false;
        }
    }

    try
    {
        // Move-constructed, never assigned: SharedSession::Guard deletes move-assign, so the whole
        // acquire finishes here while the guard is still the only holder.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) the constructor opens it or throws
        auto guard = session_->tryLock(domain, timeout);
        if (!guard)
        {
            return false;
        }
        const std::lock_guard held{heldGuard_};
        held_.emplace(domain, Held_t{std::move(*guard), taker});
        return true;
    }
    catch (const cme::Error& refused)
    {
        throw TurnUnavailable{std::string{uri_}.append("/").append(domain).append(": ").append(refused.what())};
    }
}

bool CmeTurn::tryAcquireHere(const std::string& domain)
{
    return takeDomain(domain, TryBudget, Taker::Loop);
}

void CmeTurn::awaitDomain(const std::string& domain, std::chrono::nanoseconds timeout, Answer answer)
{
    post(
        [this, domain, timeout, answer = std::move(answer)]
        {
            std::int32_t status = 0;
            try
            {
                status = takeDomain(domain, timeout, Taker::Waiter) ? 0 : -EBUSY;
            }
            catch (const TurnUnavailable&)
            {
                status = -ENOENT;
            }
            answer(status);
        });
}

bool CmeTurn::release(const std::string& domain)
{
    {
        const std::lock_guard guard{heldGuard_};
        const auto found = held_.find(domain);
        if (found == held_.end())
        {
            // An unlock for a domain this node never held is not an error: a repeat reads as this.
            return false;
        }
        if (found->second.taker == Taker::Loop)
        {
            held_.erase(found);
            return true;
        }
        if (found->second.releasing)
        {
            return false;
        }
        found->second.releasing = true;
    }

    // The waiter took it, so the waiter gives it back. The answer is already known: the entry was
    // there a moment ago, and only this unlock takes it out.
    post(
        [this, domain]
        {
            const std::lock_guard guard{heldGuard_};
            held_.erase(domain);
        });
    return true;
}

bool CmeTurn::holds(const std::string& domain) const
{
    const std::lock_guard guard{heldGuard_};
    return held_.find(domain) != held_.end();
}

void CmeTurn::post(std::function<void()> work)
{
    {
        const std::lock_guard guard{queueGuard_};
        work_.push(std::move(work));
    }
    ready_.notify_one();
}

void CmeTurn::runWaiter()
{
    for (;;)
    {
        std::function<void()> work;
        {
            std::unique_lock guard{queueGuard_};
            ready_.wait(guard, [this]
                        {
                            return stopping_ || !work_.empty();
                        });
            if (stopping_ && work_.empty())
            {
                return;
            }
            work = std::move(work_.front());
            work_.pop();
        }
        work();
    }
}

void CmeTurn::stopWaiter() noexcept
{
    {
        const std::lock_guard guard{queueGuard_};
        stopping_ = true;
    }
    ready_.notify_one();
    if (waiter_.joinable())
    {
        waiter_.join();
    }
}

void CmeTurn::openSession(const std::vector<std::string>& domains)
{
    try
    {
        session_ = cme::SharedSession::open(uri_);
    }
    catch (const cme::Error& refused)
    {
        throw TurnUnavailable{uri_ + ": " + refused.what()};
    }
    for (const auto& domain : domains)
    {
        ensureDomain(domain);
    }
}

void CmeTurn::ensureDomain(const std::string& domain)
{
    if (!session_)
    {
        throw TurnUnavailable{uri_ + ": no open session"};
    }
    try
    {
        session_->createDomain(domain);
    }
    catch (const cme::DomainExistsError&)
    {
        session_->joinDomain(domain);
    }
}

}  // namespace fsdaemon::turn
