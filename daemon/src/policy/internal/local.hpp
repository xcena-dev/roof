// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/internal/local.hpp -- the in-process rego backend, over the regorus C FFI.
//
// One base engine is loaded with the policy and never mutated after. Each evaluation clones it under
// a lock and evals on the clone, so workers evaluate in parallel with no shared mutable state. A
// decision is cached by its whole input, so a repeated consumer-owner pair is a lookup, not an eval.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "model.hpp"
#include "policy/base.hpp"
#include "regorus/engine.hpp"

namespace fsdaemon::policy
{

class LocalPolicyEngine : public PolicyEngine
{
public:
    // Loads @policyPath into the base engine. Throws std::runtime_error when the file cannot be read
    // or the rego will not compile, so a broken policy is a failure to start rather than a silent
    // deny-all. @query is the rule to evaluate, e.g. data.<fs>.authz.
    LocalPolicyEngine(std::string policyPath, std::string query);
    ~LocalPolicyEngine() override;

    [[nodiscard]] Decision_t decide(const fsdaemon::Identity_t& consumer, std::string_view ownerGroup,
                                    std::string_view ownerRole) override;
    void warm(const std::vector<fsdaemon::Identity_t>& identities) override;
    void reload() override;

    // The query a deployment gets unless its config names another: data.<fs>.authz.
    [[nodiscard]] static std::string getDefaultQuery();

private:
    // The result JSON for @inputJson, or empty on any evaluation error.
    [[nodiscard]] std::string evaluate(const std::string& inputJson);

    std::string policyPath_;
    std::string query_;

    std::mutex baseGuard_;
    regorus::Engine base_;

    // Bumped by reload() before it clears cache_, so a decide() that cloned base_ before the reload
    // can tell its answer belongs to a superseded generation and skip caching it.
    std::atomic<std::uint64_t> generation_{0};

    std::mutex cacheGuard_;
    std::unordered_map<std::string, Decision_t> cache_;
};

}  // namespace fsdaemon::policy
