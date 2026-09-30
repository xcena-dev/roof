// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/local_backend.cpp -- the name "local" answers to the in-process rego engine.

#include <memory>

#include "backends.hpp"
#include "config/config.hpp"
#include "policy/base.hpp"
#include "policy/internal/local.hpp"

namespace
{

const auto RegisteredLocalPolicy = fsdaemon::registerPolicyBackend(
    "local",
    [](const fsdaemon::config::Config& config) -> std::unique_ptr<fsdaemon::policy::PolicyEngine>
    {
        if (config.getPolicyPath().empty())
        {
            return nullptr;
        }
        return std::make_unique<fsdaemon::policy::LocalPolicyEngine>(
            config.getPolicyPath(), config.getPolicyQuery().empty()
                                        ? fsdaemon::policy::LocalPolicyEngine::getDefaultQuery()
                                        : config.getPolicyQuery());
    });

}  // namespace
