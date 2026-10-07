// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/local_backend.cpp -- the name "local" answers to the yaml rules file backend.

#include <memory>

#include "backends.hpp"
#include "config/config.hpp"
#include "identity/base.hpp"
#include "identity/internal/local.hpp"

namespace
{

const bool RegisteredLocalIdentity = fsdaemon::registerIdentityBackend(
    "local",
    [](const fsdaemon::config::Config& config) -> std::unique_ptr<fsdaemon::identity::IdentityProvider>
    {
        return std::make_unique<fsdaemon::identity::LocalIdentityProvider>(
            config.getIdentityRulesPath(), config.getSelectors(), config.getTrustDomain());
    });

}  // namespace
