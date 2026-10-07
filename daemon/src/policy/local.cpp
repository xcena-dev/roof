// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/local.cpp -- see internal/local.hpp.

#include "policy/internal/local.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model.hpp"
#include "name.hpp"
#include "policy/base.hpp"
#include "regorus/engine.hpp"
#include "util/file_text.hpp"
#include "util/text.hpp"

namespace fsdaemon::policy
{

namespace
{

// rego resolves a rule through the document tree, so a query naming only the package answers
// nothing and the evaluation fails. An administrator writes the package, which is what the same
// name means everywhere else this daemon reads it.
std::string qualifyQuery(std::string query)
{
    if (util::hasPrefix(query, "data."))
    {
        return query;
    }
    return "data." + query;
}

// The engine the policy at @policyPath builds, and the bytes it read. Throws naming that path.
std::pair<regorus::Engine, std::uint64_t> buildBase(const std::string& policyPath)
{
    const auto found = util::readTrustedText(policyPath);
    if (found.standing == util::Standing::Absent)
    {
        throw std::runtime_error{"policy: cannot read " + policyPath};
    }
    if (found.standing == util::Standing::Untrusted)
    {
        throw std::runtime_error{"policy: " + policyPath + " has unsafe permissions"};
    }
    try
    {
        return {regorus::Engine{"policy.rego", found.text}, found.text.size()};
    }
    catch (const std::runtime_error& bad)
    {
        throw std::runtime_error{std::string{"policy: "}.append(policyPath).append(": ").append(bad.what())};
    }
}

// buildBase, saying how many bytes it read, which is what an edit changes.
regorus::Engine loadBase(const std::string& policyPath)
{
    auto [engine, bytes] = buildBase(policyPath);
    std::fprintf(stderr, "[policy-local] loaded %s (%" PRIu64 " bytes)\n", policyPath.c_str(), bytes);
    return std::move(engine);
}

}  // namespace

LocalPolicyEngine::LocalPolicyEngine(std::string policyPath, std::string query)
    : policyPath_{std::move(policyPath)},
      query_{qualifyQuery(std::move(query))},
      base_{loadBase(policyPath_)}
{
}

LocalPolicyEngine::~LocalPolicyEngine() = default;

std::string LocalPolicyEngine::getDefaultQuery()
{
    return "data." + std::string{name::FsName} + ".authz";
}

std::string LocalPolicyEngine::evaluate(const std::string& inputJson)
{
    std::optional<regorus::Engine> clone;
    try
    {
        const std::lock_guard guard{baseGuard_};
        clone.emplace(base_);
    }
    catch (const std::runtime_error&)
    {
        return "";
    }
    return clone->evaluate(inputJson, query_);
}

Decision_t LocalPolicyEngine::decide(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                     std::string_view ownerRole)
{
    const auto input = buildPolicyInput(consumer, ownerGroup, ownerRole);
    {
        const std::lock_guard guard{cacheGuard_};
        if (const auto found = cache_.find(input); found != cache_.end())
        {
            return found->second;
        }
    }

    const auto generation = generation_.load(std::memory_order_acquire);
    const auto result = evaluate(input);
    const auto decision = result.empty() ? makeDenial("regorus evaluation failed")
                                         : readDecision(result);

    const std::lock_guard guard{cacheGuard_};
    // A reload since the clone above means this decision may answer for a policy nobody serves any
    // more, so it stays out of the cache rather than outliving the reload that superseded it.
    if (generation == generation_.load(std::memory_order_acquire))
    {
        cache_.emplace(input, decision);
    }
    return decision;
}

void LocalPolicyEngine::warm(const std::vector<fsdaemon::Identity_t>& identities)
{
    for (const auto& consumer : identities)
    {
        for (const auto& owner : identities)
        {
            static_cast<void>(decide(consumer, owner.group, owner.role));
        }
    }
}

void LocalPolicyEngine::checkReload() const
{
    static_cast<void>(buildBase(policyPath_));
}

void LocalPolicyEngine::reload()
{
    std::optional<regorus::Engine> rebuilt;
    try
    {
        rebuilt.emplace(loadBase(policyPath_));
    }
    catch (const std::exception& bad)
    {
        // A bad edit keeps the policy already loaded, so the daemon does not fall to deny-all under
        // a running mount.
        std::fprintf(stderr, "[policy-local] reload aborted (%s); previous policy retained\n", bad.what());
        return;
    }

    {
        const std::lock_guard guard{baseGuard_};
        base_ = std::move(*rebuilt);
    }
    // Ordered before the clear below: a decide() that reads this after seeing the clear must also
    // see the bump, so it never re-inserts what the clear just removed.
    generation_.fetch_add(1, std::memory_order_release);
    const std::lock_guard cacheHold{cacheGuard_};
    cache_.clear();
}

}  // namespace fsdaemon::policy
