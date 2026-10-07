// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/base.hpp -- the policy decision point, and the parts every backend shares.
//
// A backend evaluates a rego policy over an input the daemon builds from a consumer identity and the
// owner's group and role. Whether the rego runs in-process or on an OPA server, the input shape and
// the reading of the result are the same, so both live here and only the evaluation differs.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model.hpp"

namespace fsdaemon::policy
{

// The answer a policy gives one access request.
struct Decision_t
{
    bool allow{false};
    // Valid only when allow is true, and the bits the access response returns as granted_perms.
    std::uint32_t grantedPerms{0};
    std::int64_t expirySecs{0};
    // The rule that decided, for the audit trail, and the reason it denied when it did.
    std::string rule;
    std::string reason;
};

// A denial carrying @reason, and @rule when one was named before the refusal. Everything this layer
// refuses is fail-closed, so a denial is the only shape a caller builds by hand.
[[nodiscard]] inline Decision_t makeDenial(std::string reason, std::string rule = {})
{
    Decision_t denied{};
    denied.rule = std::move(rule);
    denied.reason = std::move(reason);
    return denied;
}

class PolicyEngine
{
public:
    PolicyEngine() = default;
    PolicyEngine(const PolicyEngine&) = delete;
    PolicyEngine(PolicyEngine&&) = delete;
    PolicyEngine& operator=(const PolicyEngine&) = delete;
    PolicyEngine& operator=(PolicyEngine&&) = delete;
    virtual ~PolicyEngine() = default;

    // The decision for @consumer against an owner's @ownerGroup and @ownerRole. Fail-closed: any
    // error in evaluation or a malformed result is a denial, never a throw.
    [[nodiscard]] virtual Decision_t decide(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                            std::string_view ownerRole) = 0;

    // Precomputes and caches the decision for every consumer-owner pair among @identities, so the
    // first real request for a known pair is a cache hit rather than an evaluation.
    virtual void warm(const std::vector<fsdaemon::Identity_t>& identities) = 0;

    // Whether a decision from this engine belongs on a worker rather than on the serve loop, which
    // is true of an engine that asks a server over the network and false of one that evaluates here.
    [[nodiscard]] virtual bool needsWorker() const noexcept
    {
        return false;
    }

    // Re-reads the policy source. A failure keeps whatever was already loaded.
    virtual void reload() = 0;

    // Throws when the next reload() would fail, and changes nothing loaded. An engine whose policy a
    // server holds has nothing here to check.
    virtual void checkReload() const
    {
    }
};

// The JSON input a decision is evaluated over: {consumer:{spiffe_id,group,role}, owner:{group,role}}.
// The same string is the cache key, so it is built once and deterministic.
[[nodiscard]] std::string buildPolicyInput(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                           std::string_view ownerRole);

// The decision @document carries, read fail-closed: a document that will not parse, a non-object, a
// missing or non-bool allow, a granted_perms that is not a list of known permission names, all deny.
// @wrapperKey names the member the decision sits under, empty when it is the whole document.
[[nodiscard]] Decision_t readDecision(std::string_view document, std::string_view wrapperKey = {});

}  // namespace fsdaemon::policy
