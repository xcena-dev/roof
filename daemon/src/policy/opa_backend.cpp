// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/opa_backend.cpp -- the name "opa" and the transport it answers with.
//
// Compiles only where libcurl does, so a build without it carries no "opa" name and refuses a config
// asking for one. The engine core beside this speaks no HTTP, so its mock test builds everywhere.

#include <memory>

#include "backends.hpp"
#include "config/config.hpp"
#include "policy/base.hpp"
#include "policy/internal/opa.hpp"
#include "policy/internal/opa_transport.hpp"

namespace fsdaemon::policy
{

namespace
{

const auto RegisteredOpaPolicy = registerPolicyBackend(
    "opa",
    [](const config::Config& config) -> std::unique_ptr<PolicyEngine>
    {
        if (config.getPolicyUrl().empty())
        {
            return nullptr;
        }
        return std::make_unique<OpaPolicyEngine>(
            config.getPolicyUrl(),
            makeCurlTransport(config.getRequestTimeout(), config.getPolicySocket()));
    });

}  // namespace

}  // namespace fsdaemon::policy
