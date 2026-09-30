// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/backends.hpp -- the backend registry: a name config spells to the thing that serves it.
//
// A backend registers itself from its own translation unit, so which backends a build carries is
// decided by which of them it compiles. No file here or above names a build switch.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "identity/base.hpp"
#include "policy/base.hpp"
#include "turn/service.hpp"

namespace fsdaemon
{

// A backend builds itself from the settings, so adding one costs a registration beside it and no
// edit here. A policy factory may return nullptr when the settings give it nothing to load.
using IdentityFactory = std::unique_ptr<identity::IdentityProvider> (*)(const config::Config&);
using PolicyFactory = std::unique_ptr<policy::PolicyEngine> (*)(const config::Config&);
using TurnFactory = std::unique_ptr<turn::TurnService> (*)(const config::Config&);

// Register @make under the name config spells for it. Each returns true, so a backend registers
// from a namespace-scope constant in its own translation unit.
bool registerIdentityBackend(std::string name, IdentityFactory make);
bool registerPolicyBackend(std::string name, PolicyFactory make);
bool registerTurnBackend(std::string name, TurnFactory make);

// The identity backend config names. Throws std::runtime_error listing what this build does carry,
// so a name it left out is refused rather than served by a default.
[[nodiscard]] std::unique_ptr<identity::IdentityProvider> makeIdentityProvider(const config::Config& config);

// The policy engine config names, warmed over what @identity can resolve to. Throws when no backend
// answers to that name, and returns nullptr only when the named one has nothing to load, which
// leaves the handler denying every access.
[[nodiscard]] std::unique_ptr<policy::PolicyEngine> makePolicyEngine(
    const config::Config& config, const identity::IdentityProvider& identity);

// The lock service for the region config names, or nullptr when it names none, no turn backend is
// in this build, or the region is down. A null service serves every request without the lock.
[[nodiscard]] std::unique_ptr<turn::TurnService> makeTurnService(const config::Config& config);

// Every backend this build carries, as "<layer>:<name>" lines. A deployment reads this to tell a
// missing backend from a misspelled one.
[[nodiscard]] std::vector<std::string> getBackendNames();

}  // namespace fsdaemon
